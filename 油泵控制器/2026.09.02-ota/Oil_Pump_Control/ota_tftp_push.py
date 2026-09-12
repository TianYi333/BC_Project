#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OTA 上位机（网关侧）一键脚本：发送 upgrade_start 命令（TCP + HMAC-SHA256 签名） + TFTP 下发 firmware.bin

协议严格对齐 UserCode/Drivers/ETH/net_comm_task.c 与 UserCode/Logic/ota.c：
  - 设备是 TCP 客户端，连接网关 TCP_SERVER_PORT（默认 50010），本脚本在此端口起 server 等设备上连。
  - upgrade_start 签名源串（设备端 tcp_sign_buf_2048，字段严格字典序）：
        {"device_id":"%s","new_crc32":%lu,"new_len":%lu,"seq":%lu,"ts":%s,"type":"upgrade_start"}
    其中 ts 为 uint64_to_str() 产生的十进制串（无前导零）。
  - HMAC_KEY 取自 UserCode/Logic/project_config.h（默认 "0123456789abcdef0123456789abcdef"，改过需同步）。
  - 设备收到命令后擦 firmware_a 并 tftp_init() 启动 TFTP server（UDP/69），仅接受文件名（由 ota.h 的 OTA_TFTP_FILENAME 宏决定，默认 "firmware.bin"；本脚本用 --tftp-filename 对应传入，须与设备端一致）。
  - 收满且 CRC 校验通过 -> 写 ota_meta(PENDING) -> 软复位 -> boot 烧录 -> App 启动后 CONFIRMED。

CRC 算法：fdb_calc_crc32 即标准 CRC-32（init=0xFFFFFFFF, 末异或 0xFFFFFFFF），与 zlib.crc32 一致。

依赖：仅 Python 标准库（socket/struct/hashlib/hmac/zlib/argparse/time）。

典型用法：
  python ota_tftp_push.py firmware.bin --device-id 1A2B3C4D5E6F
  python ota_tftp_push.py build/App.bin --device-id 1A2B3C4D5E6F --port 50010 --tftp-host 192.168.1.50
  python ota_tftp_push.py build/App.bin --device-id 1A2B3C4D5E6F --tftp-filename firmware.bin
"""

import socket
import struct
import hashlib
import hmac
import zlib
import time
import sys
import os
import argparse

# ===== 与固件保持一致的可配置项 =====
HMAC_KEY_DEFAULT = "0123456789abcdef0123456789abcdef"   # 见 project_config.h: #define HMAC_KEY
TCP_PORT_DEFAULT = 50010                                # 见 net_comm_task.h: #define TCP_SERVER_PORT
TFTP_PORT = 69
TFTP_FILENAME_DEFAULT = "Oil_Pump_Control.bin"            # 须与 ota.h 的 OTA_TFTP_FILENAME 宏一致（设备端当前即此名）


def build_sign_src(device_id, new_crc32, new_len, seq, ts):
    """复刻设备端签名源串。注意：Python 的 % 没有 %lu，用 %d 输出十进制串，等价。"""
    return ('{"device_id":"%s","new_crc32":%d,"new_len":%d,"seq":%d,"ts":%s,"type":"upgrade_start"}'
            % (device_id, new_crc32, new_len, seq, str(int(ts))))


def send_upgrade_start(conn, device_id, new_crc32, new_len, seq, ts, hmac_key):
    src = build_sign_src(device_id, new_crc32, new_len, seq, ts)
    digest = hmac.new(hmac_key.encode('ascii'), src.encode('ascii'), hashlib.sha256).digest()
    sign = ''.join('%02X' % b for b in digest[:4])   # 取前 4 字节，大写 hex，与设备端一致
    cmd = ('{"device_id":"%s","new_crc32":%d,"new_len":%d,"seq":%d,"ts":%d,'
           '"type":"upgrade_start","sign":"%s"}'
           % (device_id, new_crc32, new_len, seq, int(ts), sign))
    conn.sendall(cmd.encode('ascii'))
    return cmd, sign


def recv_until_ack(conn, keyword=b'upgrade_start_ack', total_timeout=6.0):
    conn.settimeout(1.0)
    buf = b''
    end = time.time() + total_timeout
    while time.time() < end:
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            if buf:
                break
            continue
        if not chunk:
            break
        buf += chunk
        if keyword in buf:
            break
    return buf


def tftp_put(host, data, filename=b"firmware.bin", timeout=5.0, max_retries=5):
    """极简 RFC1350 TFTP WRQ 客户端（octet 二进制模式），零依赖。

    filename: 设备端 open 回调强制校验的文件名（bytes），须与 ota.h 的 OTA_TFTP_FILENAME 一致。
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    addr = (host, TFTP_PORT)

    last_sent = struct.pack('!H', 2) + filename + b'\x00' + b'octet' + b'\x00'  # WRQ
    sock.sendto(last_sent, addr)
    block = 0
    pos = 0
    retries = 0

    while True:
        try:
            resp, _ = sock.recvfrom(1024)
        except socket.timeout:
            retries += 1
            if retries > max_retries:
                raise RuntimeError("TFTP: 重试超限，未收到 ACK（设备 TFTP server 可能未就绪）")
            sock.sendto(last_sent, addr)   # 重传最后发出的包
            continue

        if len(resp) < 4:
            raise RuntimeError("TFTP: 报文过短")
        opcode = struct.unpack('!H', resp[0:2])[0]
        ackno = struct.unpack('!H', resp[2:4])[0]

        if opcode == 5:   # ERROR
            msg = resp[4:].split(b'\x00')[0].decode(errors='replace')
            raise RuntimeError("TFTP ERROR %d: %s" % (ackno, msg))
        if opcode != 4:   # 仅处理 ACK
            raise RuntimeError("TFTP: 非法 opcode %d" % opcode)
        if ackno != block:
            continue      # 重复/乱序 ACK，忽略，等待正确序号

        # 收到正确 ACK，发送下一块 DATA
        block = (block + 1) & 0xFFFF
        chunk = data[pos:pos + 512]
        pos += len(chunk)
        last_sent = struct.pack('!HH', 3, block) + chunk   # DATA opcode=3
        sock.sendto(last_sent, addr)
        if len(chunk) < 512:
            break   # 末块（<512）或空块（恰为 512 整数倍时的 EOF 标志）

    # 等待末块的最终 ACK
    try:
        sock.recvfrom(1024)
    except socket.timeout:
        pass
    sock.close()


def main():
    ap = argparse.ArgumentParser(description="OTA 上位机：upgrade_start 命令 + TFTP 下发 .bin")
    ap.add_argument("bin", help="待升级的 firmware .bin 文件路径")
    ap.add_argument("--device-id", required=True,
                    help="设备 DEVICE_ID。见设备上电日志 \"Auto generate DEVICE_ID: XXXXXXXX\"（芯片 UID 派生，12 位 hex）")
    ap.add_argument("--port", type=int, default=TCP_PORT_DEFAULT, help="TCP 监听端口（默认 50010）")
    ap.add_argument("--seq", type=int, default=int(time.time()) % 100000,
                    help="命令序号 seq（默认取当前时间秒 %% 100000）")
    ap.add_argument("--tftp-host", default=None,
                    help="TFTP 目标 IP（默认取 TCP 对端 IP，即设备自身 IP）")
    ap.add_argument("--hmac-key", default=HMAC_KEY_DEFAULT, help="HMAC 密钥（需与 project_config.h 一致）")
    ap.add_argument("--tftp-filename", default=TFTP_FILENAME_DEFAULT,
                    help="TFTP 下发文件名（须与设备端 ota.h 的 OTA_TFTP_FILENAME 宏一致，默认 %%(default)s）")
    args = ap.parse_args()

    try:
        with open(args.bin, 'rb') as f:
            data = f.read()
    except OSError as e:
        print("无法读取固件文件: %s" % e)
        sys.exit(1)

    new_len = len(data)
    new_crc32 = zlib.crc32(data) & 0xFFFFFFFF
    print("[*] firmware : %s" % args.bin)
    print("[*] new_len  : %d (0x%X)" % (new_len, new_len))
    print("[*] new_crc32: 0x%08X" % new_crc32)
    if new_len == 0 or new_len > 0x100000:
        print("[!] 固件长度非法（须 0 < len <= 1MB），终止。")
        sys.exit(1)
    if new_crc32 == 0:
        print("[!] CRC32 为 0，设备会拒绝，终止。")
        sys.exit(1)

    # 1) 起 TCP server 等设备上连
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', args.port))
    srv.listen(1)
    print("[*] 等待设备连接 TCP %d（请确保设备已将该 IP 设为 server 并联网）..." % args.port)
    conn, addr = srv.accept()
    print("[+] 设备已连接: %s" % (addr,))
    dev_ip = addr[0]
    ts = int(time.time())

    # 2) 发送 upgrade_start
    cmd, sign = send_upgrade_start(conn, args.device_id, new_crc32, new_len, args.seq, ts, args.hmac_key)
    print("[+] 已发送 upgrade_start (seq=%d ts=%d sign=%s)" % (args.seq, ts, sign))
    print("    签名源串: %s" % build_sign_src(args.device_id, new_crc32, new_len, args.seq, ts))

    # 3) 等待 ack
    ack = recv_until_ack(conn)
    if ack:
        print("[+] 设备回: %s" % ack.decode(errors='replace').strip())
    else:
        print("[~] 未收到 upgrade_start_ack（仍尝试 TFTP；命令可能已处理，tftp_init 应已完成）")

    # 4) TFTP 下发
    tftp_host = args.tftp_host or dev_ip
    print("[*] TFTP 下发 %s -> %s:%d (文件名 %s) ..." % (args.bin, tftp_host, TFTP_PORT, args.tftp_filename))
    try:
        tftp_put(tftp_host, data, args.tftp_filename.encode('ascii'))
    except Exception as e:
        print("[!] TFTP 失败: %s" % e)
        conn.close()
        srv.close()
        sys.exit(1)

    print("[+] TFTP 完成。设备应：写 ota_meta(PENDING) -> 软复位 -> boot 烧录 -> App 启动后 ota_app_confirm() 置 CONFIRMED")
    conn.close()
    srv.close()


if __name__ == '__main__':
    main()
