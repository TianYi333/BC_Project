#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Oil Pump OTA 图形化升级工具
复用 ota_tftp_push.py 的协议实现（TCP upgrade_start + TFTP 下发）。
打包命令（在工作目录执行）：
    pyinstaller --onefile --windowed --name Oil_Pump_OTA_Tool ota_gui.py
"""

import os
import sys
import socket
import time
import zlib
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext
import threading
import queue
import traceback

# 复用现有脚本里的协议函数与默认常量
import ota_tftp_push as ota


class OTAApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Oil Pump OTA Upgrade Tool")
        self.geometry("720x560")
        self.resizable(True, True)

        # 线程通信队列
        self.log_q = queue.Queue()
        self.upgrade_thread = None
        self.stop_event = threading.Event()

        self._build_ui()
        self._periodic_log_poll()

    def _build_ui(self):
        pad = {"padx": 10, "pady": 5}

        # 标题
        ttk.Label(self, text="Oil Pump OTA 升级工具", font=("Microsoft YaHei", 14, "bold")).pack(pady=10)

        # 参数框架
        frm = ttk.Frame(self)
        frm.pack(fill=tk.X, padx=10, pady=5)
        frm.columnconfigure(1, weight=1)

        # 固件路径
        ttk.Label(frm, text="固件文件:").grid(row=0, column=0, sticky=tk.W, **pad)
        self.var_bin = tk.StringVar(value=os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "Debug", "Oil_Pump_Control.bin"))
        ttk.Entry(frm, textvariable=self.var_bin).grid(row=0, column=1, sticky=tk.EW, **pad)
        ttk.Button(frm, text="浏览...", command=self._browse).grid(row=0, column=2, **pad)

        # device-id
        ttk.Label(frm, text="Device ID:").grid(row=1, column=0, sticky=tk.W, **pad)
        self.var_device_id = tk.StringVar()
        ent_dev = ttk.Entry(frm, textvariable=self.var_device_id)
        ent_dev.grid(row=1, column=1, columnspan=2, sticky=tk.EW, **pad)
        ttk.Label(frm, text="(设备上电日志中的 12 位 hex UID)").grid(row=2, column=1, sticky=tk.W, padx=10)

        # HMAC key
        ttk.Label(frm, text="HMAC Key:").grid(row=3, column=0, sticky=tk.W, **pad)
        self.var_hmac = tk.StringVar(value=ota.HMAC_KEY_DEFAULT)
        ttk.Entry(frm, textvariable=self.var_hmac, show="*").grid(row=3, column=1, columnspan=2, sticky=tk.EW, **pad)

        # TCP port
        ttk.Label(frm, text="TCP 端口:").grid(row=4, column=0, sticky=tk.W, **pad)
        self.var_port = tk.StringVar(value=str(ota.TCP_PORT_DEFAULT))
        ttk.Entry(frm, textvariable=self.var_port, width=10).grid(row=4, column=1, sticky=tk.W, **pad)

        # TFTP host
        ttk.Label(frm, text="TFTP Host:").grid(row=5, column=0, sticky=tk.W, **pad)
        self.var_tftp_host = tk.StringVar()
        ent_host = ttk.Entry(frm, textvariable=self.var_tftp_host)
        ent_host.grid(row=5, column=1, columnspan=2, sticky=tk.EW, **pad)
        ttk.Label(frm, text="(留空则自动使用设备 TCP 对端 IP)").grid(row=6, column=1, sticky=tk.W, padx=10)

        # TFTP filename
        ttk.Label(frm, text="TFTP 文件名:").grid(row=7, column=0, sticky=tk.W, **pad)
        self.var_tftp_filename = tk.StringVar(value=ota.TFTP_FILENAME_DEFAULT)
        ttk.Entry(frm, textvariable=self.var_tftp_filename).grid(row=7, column=1, columnspan=2, sticky=tk.EW, **pad)
        ttk.Label(frm, text="(须与设备端 ota.h 的 OTA_TFTP_FILENAME 宏一致)").grid(row=8, column=1, sticky=tk.W, padx=10)

        # 按钮
        btn_frm = ttk.Frame(self)
        btn_frm.pack(fill=tk.X, padx=10, pady=10)
        self.btn_start = ttk.Button(btn_frm, text="开始升级", command=self._start_upgrade)
        self.btn_start.pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frm, text="清空日志", command=self._clear_log).pack(side=tk.LEFT, padx=5)

        # 状态栏
        self.var_status = tk.StringVar(value="就绪")
        ttk.Label(self, textvariable=self.var_status, relief=tk.SUNKEN, anchor=tk.W).pack(fill=tk.X, side=tk.BOTTOM)

        # 日志框
        ttk.Label(self, text="运行日志:").pack(anchor=tk.W, padx=10, pady=(10, 0))
        self.log_box = scrolledtext.ScrolledText(self, wrap=tk.WORD, state=tk.DISABLED, height=16)
        self.log_box.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

    def _browse(self):
        path = filedialog.askopenfilename(
            title="选择固件 .bin 文件",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")],
            initialdir=os.path.dirname(self.var_bin.get()) or os.getcwd()
        )
        if path:
            self.var_bin.set(path)

    def _log(self, msg):
        """线程安全的日志写入（任意线程可调用）。"""
        self.log_q.put(msg)

    def _periodic_log_poll(self):
        """主线程定时从队列取日志，避免 Tkinter 跨线程直接操作 UI。"""
        while not self.log_q.empty():
            try:
                msg = self.log_q.get_nowait()
                self.log_box.configure(state=tk.NORMAL)
                self.log_box.insert(tk.END, msg + "\n")
                self.log_box.see(tk.END)
                self.log_box.configure(state=tk.DISABLED)
            except queue.Empty:
                break
        self.after(100, self._periodic_log_poll)

    def _clear_log(self):
        self.log_box.configure(state=tk.NORMAL)
        self.log_box.delete(1.0, tk.END)
        self.log_box.configure(state=tk.DISABLED)

    def _start_upgrade(self):
        if self.upgrade_thread and self.upgrade_thread.is_alive():
            return
        self.stop_event.clear()
        self.btn_start.configure(state=tk.DISABLED)
        self._clear_log()
        self.var_status.set("等待设备连接...")
        self.upgrade_thread = threading.Thread(target=self._upgrade_worker, daemon=True)
        self.upgrade_thread.start()

    def _upgrade_worker(self):
        try:
            self._do_upgrade()
        except Exception as e:
            self._log("[!] 异常: %s" % e)
            self._log(traceback.format_exc())
            self.var_status.set("升级失败")
        finally:
            self.after(0, lambda: self.btn_start.configure(state=tk.NORMAL))

    def _do_upgrade(self):
        bin_path = self.var_bin.get().strip()
        device_id = self.var_device_id.get().strip()
        hmac_key = self.var_hmac.get().strip()
        tftp_filename = self.var_tftp_filename.get().strip()

        if not device_id:
            raise ValueError("Device ID 不能为空")
        if not os.path.isfile(bin_path):
            raise ValueError("固件文件不存在: %s" % bin_path)
        if not tftp_filename:
            raise ValueError("TFTP 文件名不能为空")

        try:
            port = int(self.var_port.get())
        except ValueError:
            raise ValueError("TCP 端口必须是整数")

        with open(bin_path, 'rb') as f:
            data = f.read()
        new_len = len(data)
        new_crc32 = zlib.crc32(data)

        self._log("[*] firmware : %s" % bin_path)
        self._log("[*] new_len  : %d (0x%X)" % (new_len, new_len))
        self._log("[*] new_crc32: 0x%08X" % new_crc32)
        if new_len == 0 or new_len > 0x100000:
            raise ValueError("固件长度非法（须 0 < len <= 1MB）")
        if new_crc32 == 0:
            raise ValueError("CRC32 为 0，设备会拒绝")

        # 起 TCP server
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('0.0.0.0', port))
        srv.listen(1)
        self._log("[*] 等待设备连接 TCP %d..." % port)
        self.var_status.set("等待设备连接 TCP %d" % port)

        # 用短超时 accept，支持停止事件
        srv.settimeout(1.0)
        while not self.stop_event.is_set():
            try:
                conn, addr = srv.accept()
                break
            except socket.timeout:
                continue
        else:
            srv.close()
            self.var_status.set("已取消")
            return

        self._log("[+] 设备已连接: %s" % (addr,))
        self.var_status.set("设备已连接，发送 upgrade_start...")
        dev_ip = addr[0]
        ts = int(time.time())
        seq = int(ts) % 100000

        cmd, sign = ota.send_upgrade_start(conn, device_id, new_crc32, new_len, seq, ts, hmac_key)
        self._log("[+] 已发送 upgrade_start (seq=%d ts=%d sign=%s)" % (seq, ts, sign))
        self._log("    签名源串: %s" % ota.build_sign_src(device_id, new_crc32, new_len, seq, ts))

        self.var_status.set("等待 upgrade_start_ack...")
        ack = ota.recv_until_ack(conn)
        if ack:
            self._log("[+] 设备回: %s" % ack.decode(errors='replace').strip())
        else:
            self._log("[~] 未收到 upgrade_start_ack（仍尝试 TFTP；命令可能已处理）")

        tftp_host = self.var_tftp_host.get().strip() or dev_ip
        self._log("[*] TFTP 下发 -> %s:%d (文件名 %s) ..." % (tftp_host, ota.TFTP_PORT, tftp_filename))
        self.var_status.set("TFTP 下发中...")

        ota.tftp_put(tftp_host, data, tftp_filename.encode('ascii'))

        self._log("[+] TFTP 完成。设备将写 ota_meta(PENDING) -> 软复位 -> boot 烧录 -> App 启动后 CONFIRMED")
        self.var_status.set("升级命令已下发完成")
        conn.close()
        srv.close()


def main():
    app = OTAApp()
    app.mainloop()


if __name__ == '__main__':
    main()
