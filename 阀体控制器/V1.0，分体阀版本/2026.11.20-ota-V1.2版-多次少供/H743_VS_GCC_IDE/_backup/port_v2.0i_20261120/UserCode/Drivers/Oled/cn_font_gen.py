# -*- coding: utf-8 -*-
"""
U8G2 精简中文字库生成器 (方案 A)

用途
----
为 SSD1363 256x128 OLED (U8G2) 生成只包含本项目界面实际用到字符的字体数组，
输出 cn_fonts.c / cn_fonts.h。生成的字体可直接用于：

    u8g2_SetFont(&g_u8g2, cn_font_16);
    u8g2_DrawUTF8(&g_u8g2, x, y, "系统设置");
    w = u8g2_GetUTF8Width(&g_u8g2, "系统设置");

字符来源
--------
1) ASCII 0x20..0x7E 全部收录（保证中文界面里的数字、单位、型号能混排）。
2) 中文字符由下方 TEXT_TABLE 自动去重提取；改界面文案时同步改这里，重跑本脚本即可。

取模方式
--------
ctypes 调用 Windows GDI32，在 1bpp 单色位图上用 SimSun 渲染字符后逐点读取，
纯 Python 标准库、无需 Pillow / fontTools，宋体小字号自带点阵，清晰度优于轮廓缩放。
仅可在 Windows 上运行；生成的 .c/.h 与平台无关。

字体格式
--------
严格按 u8g2_font.c 的 U8G2 新字体格式：23 字节头 + ASCII glyph 段 +
Unicode 查找表 + Unicode glyph 段；glyph_data 为 LSB-first 位域 +
(0 游程, 1 游程, continue) RLE。生成后由内置解码器复刻 C 逻辑逐像素还原校验。

注意
----
本脚本只生成字库文件，不修改任何界面代码。
"""

import ctypes
import os
import struct
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "cn_fonts.c")
OUT_H = os.path.join(HERE, "cn_fonts.h")

# ----------------------------------------------------------------------------
# 界面文案（字库覆盖目标）。汉化 oled_page.c 时，把用到的中文同步写进这里。
# ----------------------------------------------------------------------------
TEXT_TABLE = [
    # 主菜单 / 标题
    "主菜单", "主页", "系统设置", "型初始化", "注油测试", "单元测试", "关于设备", "重启",
    "设置页面", "功能开发中", "设备信息", "运行页面",
    # 系统设置二级菜单
    "串口设置", "型转速", "恢复出厂", "返回", "电机转速",
    # 操作提示
    "旋转选择，按下确认", "长按返回", "按下返回", "长按确认", "请稍候",
    "确认恢复出厂设置", "正在重启", "确认", "取消",
    # 运行状态与结果
    "初始化中", "保存成功", "失败", "成功", "就绪", "未就绪", "已停止", "运行中", "自动",
    "注油口", "状态", "角度", "设备", "版本", "温度", "湿度", "注油量", "周期", "故障",
    "联网", "离线", "在线", "停止", "开始", "完成", "错误", "警告", "忙碌", "空闲",
    # 参数与统计字段
    "油量", "次数", "时间", "日期", "编号", "地址", "模式", "当前", "剩余", "总量",
    "型号", "序号", "间隔", "阀体", "电机", "编码器", "传感器", "网络", "配置",
    "恢复出厂设置", "升级", "固件", "重启结果", "执行结果",
    # 单位与标点
    "秒", "分", "时", "次", "毫升", "度", "百分比",
    "，", "。", "：", "、", "（", "）", "！", "？", "％", "－", "～",
    # 编辑页与电机速度页（补字：上修后大改效最标波特率生目节调）
    "波特率", "设置后重启生效", "旋转选择，按下保存，长按返回",
    "最大速度", "目标速度", "旋转选择，按下修改，长按返回", "旋转调节，按下确认，长按返回",
    "上传感", "下传感",
    # 底部提示栏 / 警告 / 报错信息（running_logic.c 的 show_oled_msg）
    "复位失败", "初始化故障", "就位", "就绪", "传感", "超时",
    "解除保护", "未归零", "运行失败", "初始化", "故障", "等待", "任务", "执行",
    # 品牌标题（主页标题栏 + 开机 LOGO）
    "博驰润滑科技",
]

# 需要生成的字号与数组名
FONTS = [
    (12, "cn_font_12"),
    (16, "cn_font_16"),
    (24, "cn_font_24"),
]
FACE_NAME = "SimSun"        # 宋体：12/16px 有内嵌点阵，小字号最清晰
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

# 字体位域参数（必须与下方 write_glyph / 解码器保持一致）
# 0 游程普遍长于 1 游程，故 BP0 > BP1；超长游程由 rle_pairs 自动拆分。
BP0, BP1 = 6, 4             # RLE 游程位宽，max run = 63 / 15
BPW = BPH = 6               # glyph 宽/高位宽 (0..63)
BPX = BPY = BPD = 7         # 有符号位域位宽 (-64..63)
BIAS = 1 << (BPX - 1)       # 有符号偏置 = 64

# ----------------------------------------------------------------------------
# GDI32 单色位图取模
# ----------------------------------------------------------------------------
_gdi = ctypes.WinDLL("gdi32", use_last_error=True)
_gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]
_gdi.CreateCompatibleDC.restype = wintypes.HDC
_gdi.CreateBitmap.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint,
                              ctypes.c_uint, ctypes.c_void_p]
_gdi.CreateBitmap.restype = wintypes.HBITMAP
_gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
_gdi.SelectObject.restype = wintypes.HGDIOBJ
_gdi.DeleteObject.argtypes = [wintypes.HGDIOBJ]
_gdi.DeleteObject.restype = wintypes.BOOL
_gdi.DeleteDC.argtypes = [wintypes.HDC]
_gdi.DeleteDC.restype = wintypes.BOOL
_gdi.CreateFontW.argtypes = [ctypes.c_int] * 13 + [wintypes.LPCWSTR]
_gdi.CreateFontW.restype = wintypes.HFONT
_gdi.SetTextColor.argtypes = [wintypes.HDC, ctypes.c_uint32]
_gdi.SetTextColor.restype = ctypes.c_uint32
_gdi.SetBkMode.argtypes = [wintypes.HDC, ctypes.c_int]
_gdi.SetBkMode.restype = ctypes.c_int
_gdi.TextOutW.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                          wintypes.LPCWSTR, ctypes.c_int]
_gdi.TextOutW.restype = wintypes.BOOL
_gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
_gdi.GetPixel.restype = ctypes.c_uint32
_gdi.PatBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int,
                        ctypes.c_int, ctypes.c_int, ctypes.c_uint32]
_gdi.PatBlt.restype = wintypes.BOOL

BLACKNESS = 0x00000042
TRANSPARENT = 1
FW_NORMAL = 400
FW_BOLD = 700               # 品牌标题专用粗体（宋体小字号下 GDI 会合成加粗，已实测有效）
GB2312_CHARSET = 134


class Rasterizer(object):
    """复用一个单色 DC，逐字渲染到 size x size 的 1bpp 位图。"""

    def __init__(self, size, face=FACE_NAME, weight=FW_NORMAL):
        self.size = size
        self.hdc = _gdi.CreateCompatibleDC(0)
        self.hbmp = _gdi.CreateBitmap(size, size, 1, 1, None)
        _gdi.SelectObject(self.hdc, self.hbmp)
        self.hfont = _gdi.CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0,
                                      GB2312_CHARSET, 0, 0, 0, 0, face)
        _gdi.SelectObject(self.hdc, self.hfont)
        _gdi.SetBkMode(self.hdc, TRANSPARENT)
        _gdi.SetTextColor(self.hdc, 0x00FFFFFF)
        assert self.hdc and self.hbmp and self.hfont, "GDI32 初始化失败"

    def close(self):
        _gdi.DeleteObject(self.hfont)
        _gdi.DeleteObject(self.hbmp)
        _gdi.DeleteDC(self.hdc)

    def render(self, ch):
        """返回 size x size 的 0/1 位图（行优先，list[int]）。"""
        n = self.size
        _gdi.PatBlt(self.hdc, 0, 0, n, n, BLACKNESS)
        if not _gdi.TextOutW(self.hdc, 0, 0, ch, 1):
            raise RuntimeError("TextOutW 失败: %r" % ch)
        get = _gdi.GetPixel
        hdc = self.hdc
        return [1 if (get(hdc, x, y) & 0xFFFFFF) else 0
                for y in range(n) for x in range(n)]


# ----------------------------------------------------------------------------
# U8G2 字体位域编码
# ----------------------------------------------------------------------------
class BitWriter(object):
    """LSB-first 位流，与 u8g2_font_decode_get_unsigned_bits 完全对应。"""

    def __init__(self):
        self.buf = bytearray()
        self.pos = 0                      # 绝对位位置

    def put(self, value, cnt):
        assert 0 <= value < (1 << cnt), ("位域溢出", value, cnt)
        for i in range(cnt):
            if (value >> i) & 1:
                byte = self.pos >> 3
                while len(self.buf) <= byte:
                    self.buf.append(0)
                self.buf[byte] |= 1 << (self.pos & 7)
            else:
                byte = self.pos >> 3
                while len(self.buf) <= byte:
                    self.buf.append(0)
            self.pos += 1

    def finish(self):
        # 读取器在 bit_pos+cnt>=8 时会访问下一字节，末尾多留 2 个 0 保证安全
        while len(self.buf) <= (self.pos >> 3) + 2:
            self.buf.append(0)
        return bytes(self.buf)


def rle_pairs(bits, max_run_0, max_run_1):
    """把 0/1 位流切成 (0 游程, 1 游程) 对；超长游程自动拆成多对。"""
    pairs = []
    i, n = 0, len(bits)
    while i < n:
        a = 0
        while i < n and bits[i] == 0 and a < max_run_0:
            a += 1
            i += 1
        b = 0
        while i < n and bits[i] == 1 and b < max_run_1:
            b += 1
            i += 1
        pairs.append((a, b))
    return pairs


def write_glyph(bits, w, h, xoff, yoff, delta):
    """编码单个 glyph_data。bits 为 w*h 行优先位流（w=0 表示空字形）。"""
    bw = BitWriter()
    bw.put(w, BPW)
    bw.put(h, BPH)
    bw.put(xoff + BIAS, BPX)
    bw.put(yoff + BIAS, BPY)
    bw.put(delta + BIAS, BPD)
    if w > 0:
        for a, b in rle_pairs(bits, (1 << BP0) - 1, (1 << BP1) - 1):
            bw.put(a, BP0)
            bw.put(b, BP1)
            bw.put(0, 1)                 # continue = 0：每对独立
        # 终止对 (0,0,0)：保证解码器把列位置归一化到行首并令 y>=h 后退出
        bw.put(0, BP0)
        bw.put(0, BP1)
        bw.put(0, 1)
    return bw.finish()


def crop_columns(bitmap, size):
    """裁掉全空的左右列，返回 (宽度, 左偏移, 裁切后位流)。"""
    cols = [any(bitmap[y * size + x] for y in range(size))
            for x in range(size)]
    if not any(cols):
        return 0, 0, []
    lo = cols.index(True)
    hi = len(cols) - 1 - cols[::-1].index(True)
    out = [bitmap[y * size + x] for y in range(size) for x in range(lo, hi + 1)]
    return hi - lo + 1, lo, out


def make_glyph(ch, bitmap, size, is_cjk):
    if is_cjk:
        # 汉字固定全宽方块，delta = 字号
        w = h = size
        xoff = yoff = 0
        delta = size
        bits = bitmap
    else:
        w, xoff, bits = crop_columns(bitmap, size)
        h = size
        yoff = 0
        delta = (w + 1) if w else max(1, size // 2)
        if ch == '.':
            # 版本号 "2.2.4i" 的点号：GDI 宋体句点仅 2px 宽，默认 delta=w+1
            # 会让点号紧贴前后数字（各剩 1px）看起来粘连。这里把点号放进
            # 半个字宽的方框并居中，左右各留约 size/4 空隙，视觉上分隔清晰。
            box = max(4, size // 2)
            xoff = (box - w) // 2 if w else xoff
            delta = box
    gd = write_glyph(bits, w, h, xoff, yoff, delta)
    return gd, (w, h, bits)


# ----------------------------------------------------------------------------
# 组装字体
# ----------------------------------------------------------------------------
def build_font(size, name, cjk_chars, weight=FW_NORMAL):
    ras = Rasterizer(size, weight=weight)
    try:
        ascii_rec = []
        for enc in range(ASCII_FIRST, ASCII_LAST + 1):
            ch = chr(enc)
            gd, ref = make_glyph(ch, ras.render(ch), size, False)
            total = 2 + len(gd)
            assert total <= 255, ("ASCII glyph 超长", ch, total)
            ascii_rec.append((enc, bytes([enc, total]) + gd, ref))

        uni_rec = []
        for cp in sorted(cjk_chars):
            ch = chr(cp)
            gd, ref = make_glyph(ch, ras.render(ch), size, True)
            total = 3 + len(gd)
            assert total <= 255, ("Unicode glyph 超长", ch, hex(cp), total)
            uni_rec.append((cp, bytes([cp >> 8, cp & 0xFF, total]) + gd, ref))
    finally:
        ras.close()

    ascii_section = b"".join(r for _, r, _ in ascii_rec) + b"\x00\x00"
    start_upper_A = sum(len(r) for e, r, _ in ascii_rec if e < ord("A"))
    start_lower_a = sum(len(r) for e, r, _ in ascii_rec if e < ord("a"))
    start_unicode = len(ascii_section)

    # 查找表只放一项：delta=表长 指向 glyph 段，e=0xFFFF 使 do-while 立即退出
    lookup = struct.pack(">HH", 4, 0xFFFF)
    uni_section = b"".join(r for _, r, _ in uni_rec) + b"\x00\x00\x00"

    glyph_cnt = len(ascii_rec) + len(uni_rec)
    # U8G2 运行时仅把 header[0] 读入 font_info.glyph_cnt 作信息字段，
    # 从不用于字符查找边界（查找靠 Unicode 索引表 0xFFFF 与段尾 0x00 标记）。
    # 该字段为 uint8_t，超过 255 时饱和写入，避免溢出回绕误导字体查看工具。
    glyph_cnt_field = min(glyph_cnt, 255)
    header = bytes([
        glyph_cnt_field,                 # 0
        0,                               # 1 bbx_mode
        BP0, BP1,                        # 2,3
        BPW, BPH, BPX, BPY, BPD,         # 4..8
        size, size,                      # 9,10 max_char_width/height
        0, 0,                            # 11,12 x/y offset
        size - 1, 0, size - 1, 0,        # 13..16 ascent_A/descent_g/ascent_para/descent_para
    ]) + struct.pack(">HHH", start_upper_A, start_lower_a, start_unicode)

    font = header + ascii_section + lookup + uni_section
    refs = {}
    refs.update({e: r for e, _, r in ascii_rec})
    refs.update({cp: r for cp, _, r in uni_rec})
    return name, font, refs, len(uni_rec)


# ----------------------------------------------------------------------------
# 独立解码器：严格复刻 u8g2_font.c，用于逐像素自校验
# ----------------------------------------------------------------------------
class BitReader(object):
    def __init__(self, data):
        self.d = data
        self.p = 0
        self.bp = 0

    def get(self, cnt):
        val = self.d[self.p] >> self.bp
        if self.bp + cnt >= 8:
            val |= self.d[self.p + 1] << (8 - self.bp)
            self.p += 1
            self.bp = self.bp + cnt - 8
        else:
            self.bp += cnt
        return val & ((1 << cnt) - 1)


def decode_glyph_data(gd, info):
    """返回 (w, h, bits)，复刻 u8g2_font_setup_decode + decode_glyph + decode_len。"""
    br = BitReader(gd)
    w = br.get(info["BPW"])
    h = br.get(info["BPH"])
    _x = br.get(info["BPX"]) - (1 << (info["BPX"] - 1))
    _y = br.get(info["BPY"]) - (1 << (info["BPY"] - 1))
    _d = br.get(info["BPD"]) - (1 << (info["BPD"] - 1))
    out = [0] * (w * h) if w else []
    if w == 0:
        return w, h, out

    lx = ly = 0

    def run(cnt, fg):
        nonlocal lx, ly
        while True:
            rem = w - lx
            cur = rem if cnt >= rem else cnt
            if fg:
                for k in range(cur):
                    idx = ly * w + lx + k
                    if idx < len(out):
                        out[idx] = 1
            if cnt < rem:
                lx += cnt
                return
            cnt -= rem
            lx = 0
            ly += 1

    guard = 0
    while True:
        guard += 1
        assert guard < 20000, "解码未终止（位流或 RLE 有误）"
        a = br.get(info["BP0"])
        b = br.get(info["BP1"])
        while True:
            run(a, 0)
            run(b, 1)
            if br.get(1) == 0:
                break
        if ly >= h:
            break
    return w, h, out


def find_glyph(font, encoding):
    """严格复刻 u8g2_font_get_glyph_data 的查找路径（含多段 Unicode 索引表）。"""
    start_A, start_a, start_uni = struct.unpack(">HHH", font[17:23])
    base = 23
    if encoding <= 255:
        p = base
        if encoding >= ord("a"):
            p += start_a
        elif encoding >= ord("A"):
            p += start_A
        while True:
            if font[p + 1] == 0:
                return None
            if font[p] == encoding:
                return font[p + 2:p + font[p + 1]]
            p += font[p + 1]
    else:
        table = base + start_uni
        p = table
        # do { font += delta; e = table_e; table += 4; } while (e < encoding)
        while True:
            p += struct.unpack_from(">H", font, table)[0]
            e = struct.unpack_from(">H", font, table + 2)[0]
            table += 4
            if e >= encoding:
                break
        while True:
            e = (font[p] << 8) | font[p + 1]
            if e == 0:
                return None
            if e == encoding:
                return font[p + 3:p + font[p + 2]]
            p += font[p + 2]


def verify_font(name, font, refs):
    # 参数一律从生成结果的头部读取，避免"编码器/解码器共享同一个错误常量"
    info = {"BP0": font[2], "BP1": font[3], "BPW": font[4], "BPH": font[5],
            "BPX": font[6], "BPY": font[7], "BPD": font[8]}
    assert len(font) >= 23 + 4
    for enc, (w, h, want) in refs.items():
        gd = find_glyph(font, enc)
        assert gd is not None, ("字形查找失败", name, hex(enc))
        gw, gh, got = decode_glyph_data(gd, info)
        assert (gw, gh) == (w, h), ("尺寸不符", name, hex(enc), (gw, gh), (w, h))
        assert got == want, ("位图不符", name, hex(enc))
    return len(refs)


# ----------------------------------------------------------------------------
# 输出 C 文件
# ----------------------------------------------------------------------------
def hex_lines(data, per_line=16):
    out = []
    for i in range(0, len(data), per_line):
        out.append("    " + ", ".join("0x%02X" % b for b in data[i:i + per_line]) + ",")
    if out:
        out[-1] = out[-1].rstrip(",")
    return out


def _byte_line(data, indent="    "):
    return indent + ", ".join("0x%02X" % b for b in data) + ","


def _split_glyphs(font):
    """按 U8G2 字体格式切出各段。
    返回 (header, ascii_glyphs, tail2, lookup, uni_glyphs, tail_end)；
    各段按顺序原样拼接必须等于 font（由调用前校验保证无字节丢失）。
    ASCII 记录 [enc, total, gd...]；Unicode 记录 [hi, lo, total, gd...]。"""
    base = 23
    header = font[:base]
    # ASCII 段：从 base 起，下一记录长度 = font[p+1]，total==0 结束
    p = base
    ascii_glyphs = []
    while p + 1 < len(font) and font[p + 1] != 0:
        enc, total = font[p], font[p + 1]
        ascii_glyphs.append((enc, font[p:p + total]))
        p += total
    # 结尾 0x00 0x00 的 2 字节 + 4 字节 Unicode 索引表
    tail2 = font[p:p + 2]
    lookup = font[p + 2:p + 2 + 4]
    q = p + 2 + 4
    uni_glyphs = []
    while q + 2 < len(font):
        code = (font[q] << 8) | font[q + 1]
        if code == 0:
            tail_end = font[q:]
            break
        total = font[q + 2]
        uni_glyphs.append((code, font[q:q + total]))
        q += total
    else:
        tail_end = b""
    return header, ascii_glyphs, tail2, lookup, uni_glyphs, tail_end


def emit_c(fonts, cjk_chars):
    lines = []
    lines.append("/*")
    lines.append(" * cn_fonts.c - U8G2 精简中文点阵字库（自动生成，请勿手工编辑）")
    lines.append(" * 由 cn_font_gen.py 生成；重新生成：python cn_font_gen.py")
    lines.append(" *")
    lines.append(" * 内容：ASCII 0x20-0x7E 全量 + %d 个界面用中文字符/标点" % len(cjk_chars))
    for size, name, font, n_uni in fonts:
        lines.append(" * %-12s %2dx%-2d  %5d B  (ASCII 95 + 中文 %d)" %
                     (name, size, size, len(font), n_uni))
    lines.append(" *")
    lines.append(" * 【为什么精简】OLED 单色屏，一个字就是一张 0/1 点阵图。全量 GB2312 约 216KB 装不下，")
    lines.append(" *              故只收界面实际用到的字，RLE 压缩后约 40KB 烧进 Flash。")
    lines.append(" * 【两段流程】离线(GDI32 宋体取模→压缩→本数组)；运行时(按码点查表→解码→逐像素点亮)。")
    lines.append(" *")
    lines.append(" * 【数组布局】一维 uint8_t，顺序排 6 段，没有随机索引目录：")
    lines.append(" *   ① 头 23B：位宽/ascent/三个段偏移(start_A、start_a、start_unicode)")
    lines.append(" *   ② ASCII 段 95 个：每块 [编码1B][total1B][数据...]")
    lines.append(" *   ③ 结束标记 00 00")
    lines.append(" *   ④ Unicode 索引表 4B：[delta=4 到字形段][FFFF 结束]")
    lines.append(" *   ⑤ Unicode 字形段：每块 [码点高1B][码点低1B][total1B][数据...]，按码点升序")
    lines.append(" *   ⑥ 结束标记 00 00 00")
    lines.append(" *")
    lines.append(" * 【如何查找】u8g2_font.c:u8g2_font_get_glyph_data() 顺序扫描+长度跳跃，无哈希/二分：")
    lines.append(" *   ASCII 用 start_A/start_a 跳到起点逐块比编码；汉字经索引表 delta 进字形段比码点。")
    lines.append(" *   total 含块头(ASCII=2+数据，汉字=3+数据)：不匹配时 font+=total 正好到下一块起点；")
    lines.append(" *   命中则 return font+2/font+3 跳过头部，返回连续数据的首字节。")
    lines.append(" *   扫到结束标记仍无命中则返回 NULL→u8g2 不画，故字库缺字显示为空。")
    lines.append(" *   头字节 glyph_cnt 饱和写 255：查找靠索引表+结束标记，不依赖它，多字形仍安全。")
    lines.append(" *")
    lines.append(" * 【用法】u8g2_SetFont(&g_u8g2, cn_font_16); u8g2_DrawUTF8(...);")
    lines.append(" *        宽度用 u8g2_GetUTF8Width()。DrawStr/GetStrWidth 只按 ASCII 解码，中文必须 UTF8 版。")
    lines.append(" * 字形来源：Windows GDI32 + SimSun 1bpp 单色位图取模。")
    lines.append(" */")
    lines.append("")
    lines.append('#include "cn_fonts.h"')
    lines.append("")
    for size, name, font, n_uni in fonts:
        lines.append("/** @brief %dx%d 精简中文字库（ASCII + %d 个中文字符） */" %
                     (size, size, n_uni))
        lines.append("const uint8_t %s[%d] = {" % (name, len(font)))
        header, ascii_glyphs, tail2, lookup, uni_glyphs, tail_end = _split_glyphs(font)
        # ---- 23 字节字体头 ----
        lines.append("    /* ---- 字体头 23B：数量/位宽/ascent/段偏移 ---- */")
        lines.append(_byte_line(header))
        # ---- ASCII 95 个（每段首字节即 ASCII 码）----
        lines.append("    /* ---- ASCII 0x20-0x7E（95 个，变宽）---- */")
        lines.append("    /* 每块：[编码1B][total1B][字形数据…]；total=2+数据长，不含下一块 */")
        for enc, rec in ascii_glyphs:
            ch = chr(enc)
            label = " " + ch if 0x21 <= enc <= 0x7E else ""
            lines.append("    /* 0x%02X = %d%s */" % (enc, enc, label))
            lines.append(_byte_line(rec))
        lines.append(_byte_line(tail2))
        # ---- Unicode 索引表（4B）----
        lines.append("    /* ---- Unicode 索引表 4B：[delta 跳到字形段][FFFF 收尾] ---- */")
        lines.append(_byte_line(lookup))
        # ---- Unicode 147 个汉字/标点 ----
        lines.append("    /* ---- Unicode 字形段（%d 个汉字/标点，全宽）---- */" % n_uni)
        lines.append("    /* 每块：[码点高1B][码点低1B][total1B][字形数据…]；total=3+数据长；按码点升序 */")
        for code, rec in uni_glyphs:
            ch = chr(code)
            lines.append("    /* U+%04X %s */" % (code, ch))
            lines.append(_byte_line(rec))
        if tail_end:
            lines.append(_byte_line(tail_end))
        lines.append("};")
        lines.append("")
    return "\r\n".join(lines) + "\r\n"


def emit_h(fonts):
    lines = []
    lines.append("/*")
    lines.append(" * cn_fonts.h - U8G2 精简中文点阵字库声明（自动生成，请勿手工编辑）")
    lines.append(" */")
    lines.append("")
    lines.append("#ifndef CN_FONTS_H")
    lines.append("#define CN_FONTS_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("#include \"u8g2.h\"")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")
    for size, name, font, n_uni in fonts:
        lines.append("/** @brief %2dx%-2d 点阵，汉字固定全宽，ASCII 变宽 */" % (size, size))
        lines.append("extern const uint8_t %s[];" % name)
    lines.append("")
    lines.append("/* 配套用法：")
    lines.append(" *   u8g2_SetFont(&g_u8g2, cn_font_16);")
    lines.append(' *   u8g2_DrawUTF8(&g_u8g2, x, y, "系统设置");')
    lines.append(' *   w = u8g2_GetUTF8Width(&g_u8g2, "系统设置");')
    lines.append(" * 注意：u8g2_DrawStr / u8g2_GetStrWidth 只按 ASCII 解码，中文必须用 UTF8 版。")
    lines.append(" */")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* CN_FONTS_H */")
    return "\r\n".join(lines) + "\r\n"


def main():
    cjk_chars = set()
    for text in TEXT_TABLE:
        for ch in text:
            if ord(ch) > 0x7F:
                cjk_chars.add(ord(ch))
    print("界面中文字符/标点 = %d 个" % len(cjk_chars))
    print("ASCII = %d 个" % (ASCII_LAST - ASCII_FIRST + 1))

    fonts = []
    for size, name in FONTS:
        name, font, refs, n_uni = build_font(size, name, cjk_chars)
        n = verify_font(name, font, refs)
        print("%-12s %2dx%-2d  %6d B  ASCII+中文=%d  解码逐像素校验通过" %
              (name, size, size, len(font), n))
        fonts.append((size, name, font, n_uni))

    # 品牌标题专用粗体字库：仅收「博驰润滑科技」6 字，16px + FW_BOLD。
    # 只供主页标题栏与开机 LOGO 使用，不影响其它页面的常规字重。
    title_chars = set(ord(c) for c in "博驰润滑科技")
    name, font, refs, n_uni = build_font(16, "cn_font_16_bold", title_chars,
                                         weight=FW_BOLD)
    n = verify_font(name, font, refs)
    print("%-12s %2dx%-2d  %6d B  ASCII+中文=%d  解码逐像素校验通过" %
          (name, 16, 16, len(font), n))
    fonts.append((16, name, font, n_uni))

    with open(OUT_C, "wb") as f:
        f.write(emit_c(fonts, cjk_chars).encode("utf-8"))
    with open(OUT_H, "wb") as f:
        f.write(emit_h(fonts).encode("utf-8"))
    print("已生成: %s" % OUT_C)
    print("已生成: %s" % OUT_H)
    print("三套合计: %d B (%.1f KB)" %
          (sum(len(x[2]) for x in fonts), sum(len(x[2]) for x in fonts) / 1024.0))


if __name__ == "__main__":
    main()
