#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GT-DOS 字模提取脚本 (原生尺寸灰度抗锯齿 + 基线对齐版)
=======================================================================
用 Windows GDI 从系统 TrueType 字体提取 16x16 灰度字模:
  - 原生 14px 渲染 + ANTIALIASED_QUALITY: GDI 在实际尺寸上做 hinting,
    笔画横平竖直 (清晰), 只有边缘是灰度 (圆滑). 不做超采样降采样,
    那会破坏 hinting 导致整体发虚.
  - 4bit 灰度 (16 级): 边缘过渡细腻.
  - 基线对齐: 用 TEXTMETRIC.tmAscent 计算真实基线, 所有字模
    (中文/英文/符号) 基线固定在输出第 13 行 -> 混排排列平齐
输出:
  build/font.bin  每字模 128 字节: 16 行 x 8 字节, 每行 16 像素 x 4bit
                  灰度 (0=背景 15=实心), 行内 MSB first (高 4bit=偶数列)
  build/cpm.bin   码点表, u16 小端升序 (内核二分查找用)
用法: python tools/mkfont.py   (build.py 会在缺失时自动调用)
"""

import ctypes
import os
import struct
import sys

g = ctypes.windll.gdi32

g.CreateCompatibleDC.restype = ctypes.c_void_p
g.CreateCompatibleDC.argtypes = [ctypes.c_void_p]
g.CreateDIBSection.restype = ctypes.c_void_p
g.CreateDIBSection.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.c_uint, ctypes.c_void_p,
                               ctypes.c_void_p, ctypes.c_uint]
g.SelectObject.restype = ctypes.c_void_p
g.SelectObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
g.CreateFontW.restype = ctypes.c_void_p
g.CreateFontW.argtypes = [ctypes.c_int] * 6 + [ctypes.c_uint] * 7 + \
                         [ctypes.c_wchar_p]
g.PatBlt.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                     ctypes.c_int, ctypes.c_int, ctypes.c_uint]
g.SetBkColor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
g.SetTextColor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
g.SetTextAlign.argtypes = [ctypes.c_void_p, ctypes.c_uint]
g.ExtTextOutW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                          ctypes.c_uint, ctypes.c_void_p, ctypes.c_wchar_p,
                          ctypes.c_uint, ctypes.c_void_p]
g.GetTextMetricsW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
g.DeleteObject.argtypes = [ctypes.c_void_p]
g.DeleteDC.argtypes = [ctypes.c_void_p]


class BIH(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint), ("w", ctypes.c_long),
                ("h", ctypes.c_long), ("planes", ctypes.c_ushort),
                ("bitcnt", ctypes.c_ushort), ("comp", ctypes.c_uint),
                ("imgsize", ctypes.c_uint), ("xppm", ctypes.c_long),
                ("yppm", ctypes.c_long), ("clrused", ctypes.c_uint),
                ("clrimp", ctypes.c_uint)]


class TEXTMETRICW(ctypes.Structure):
    _fields_ = [("tmHeight", ctypes.c_long),
                ("tmAscent", ctypes.c_long),
                ("tmDescent", ctypes.c_long),
                ("tmInternalLeading", ctypes.c_long),
                ("tmExternalLeading", ctypes.c_long),
                ("tmAveCharWidth", ctypes.c_long),
                ("tmMaxCharWidth", ctypes.c_long),
                ("tmWeight", ctypes.c_long),
                ("tmOverhang", ctypes.c_long),
                ("tmDigitizedAspectX", ctypes.c_long),
                ("tmDigitizedAspectY", ctypes.c_long),
                ("tmFirstChar", ctypes.c_wchar),
                ("tmLastChar", ctypes.c_wchar),
                ("tmDefaultChar", ctypes.c_wchar),
                ("tmBreakChar", ctypes.c_wchar),
                ("tmItalic", ctypes.c_ubyte),
                ("tmUnderlined", ctypes.c_ubyte),
                ("tmStruckOut", ctypes.c_ubyte),
                ("tmPitchAndFamily", ctypes.c_ubyte),
                ("tmCharSet", ctypes.c_ubyte)]


CELL = 16            # 输出单元 16x16
DIB = 32             # 渲染画布 (留边距防裁切)
BYTES_PER_GLYPH = CELL * 8           # 128 字节: 16 行 x 8 字节 (4bit/像素)
BLACKNESS = 0x00000042
FONT_EM = -14        # 原生渲染 em (14 像素, 16 格内留 1px 上下边距)
RENDER_X = 6
RENDER_Y = 8
BASELINE_OUT = 13    # 基线固定在输出第 13 行上方边界 (行 0..12=上伸区)
# CLEARTYPE_QUALITY 渲染, 只读绿色通道 = 带 hinting 的灰度 AA (实心笔画+平滑边缘)
CLEARTYPE_QUALITY = 5

FONT_CANDIDATES = ["SimHei", "Microsoft YaHei", "SimSun", "FangSong"]
# ASCII 0x20..0x7E + CJK 标点 + 汉字 + 全角, 升序
CODES = (list(range(0x20, 0x7F)) +
         list(range(0x3000, 0x3040)) +
         list(range(0x4E00, 0xA000)) +
         list(range(0xFF00, 0xFFF0)))


def render_cover(mem, buf, cp):
    """渲染码点到 DIB 画布, 返回 32x32 覆盖率 (0..255, 绿色通道)."""
    g.PatBlt(mem, 0, 0, DIB, DIB, BLACKNESS)
    g.ExtTextOutW(mem, RENDER_X, RENDER_Y, 0, None, chr(cp), 1, None)
    cov = bytearray(DIB * DIB)
    for y in range(DIB):
        base = y * DIB * 4 + 1
        row = y * DIB
        for x in range(DIB):
            cov[row + x] = buf[base + x * 4]
    return cov


def ink_bbox(cov):
    """墨迹包围盒 (l,t,r,b), 无墨迹返回 None. 阈值 16 滤掉淡边."""
    l, t, r, b = DIB, DIB, -1, -1
    for y in range(DIB):
        row = y * DIB
        hit = False
        for x in range(DIB):
            if cov[row + x] > 16:
                hit = True
                if x < l:
                    l = x
                if x > r:
                    r = x
        if hit:
            if y < t:
                t = y
            if y > b:
                b = y
    if r < l:
        return None
    return l, t, r, b


def crop4bit(cov, ox, oy):
    """以画布 (ox,oy) 为左上角取 16x16, 8bit 覆盖 -> 4bit 灰度打包."""
    cell = bytearray(BYTES_PER_GLYPH)
    for y in range(CELL):
        sy = oy + y
        if sy < 0 or sy >= DIB:
            continue
        row = sy * DIB
        for x in range(CELL):
            sx = ox + x
            if sx < 0 or sx >= DIB:
                continue
            c = cov[row + sx]
            if c < 16:
                continue
            lvl = (c + 8) >> 4
            if lvl > 15:
                lvl = 15
            cell[y * 8 + (x >> 1)] |= lvl << (4 if (x & 1) == 0 else 0)
    return cell


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = os.path.join(root, "build")
    os.makedirs(out_dir, exist_ok=True)

    if sys.platform != "win32":
        print("[!] 本脚本需要 Windows GDI")
        return 1

    bi = BIH()
    bi.size = ctypes.sizeof(bi)
    bi.w = DIB
    bi.h = -DIB
    bi.planes = 1
    bi.bitcnt = 32
    bi.comp = 0
    pp = ctypes.c_void_p()
    mem = g.CreateCompatibleDC(None)
    hb = g.CreateDIBSection(None, ctypes.byref(bi), 0,
                            ctypes.byref(pp), None, 0)
    if not hb or not pp.value:
        print("[!] CreateDIBSection 失败")
        return 1
    g.SelectObject(mem, hb)
    buf = (ctypes.c_ubyte * (DIB * DIB * 4)).from_address(pp.value)

    g.SetTextColor(mem, 0x00FFFFFF)     # 前景白 (绿色通道=覆盖率)
    g.SetBkColor(mem, 0x00000000)
    g.SetTextAlign(mem, 0)              # TA_LEFT | TA_TOP

    font = None
    name_used = None
    tm = TEXTMETRICW()
    for name in FONT_CANDIDATES:
        h = g.CreateFontW(FONT_EM, 0, 0, 0, 400, 0, 0, 0, 134,
                          0, 0, CLEARTYPE_QUALITY, 0x21, name)
        if not h:
            continue
        g.SelectObject(mem, h)
        if any(render_cover(mem, buf, 0x4E2D)) and g.GetTextMetricsW(mem, ctypes.byref(tm)):
            font = h
            name_used = name
            break
        g.DeleteObject(h)
    if not font:
        print("[!] 未找到可用中文字体")
        return 1

    # 基线 (画布坐标): TA_TOP 下, 行盒顶在 RENDER_Y, 基线 = RENDER_Y + tmAscent
    baseline_cy = RENDER_Y + tm.tmAscent
    # 画布基线 -> 输出第 BASELINE_OUT 行上边界: 裁剪窗口左上角 oy
    oy = baseline_cy - BASELINE_OUT
    print(f"    字体: {name_used}  ascent={tm.tmAscent} descent={tm.tmDescent}")
    print(f"    基线: 画布 y={baseline_cy} -> 输出行 {BASELINE_OUT} (原生灰度 AA)")

    font_bin = bytearray()
    blank = 0
    for i, cp in enumerate(CODES):
        cov = render_cover(mem, buf, cp)
        bbox = ink_bbox(cov)
        if bbox is None:
            font_bin += bytes(BYTES_PER_GLYPH)
            blank += 1
            continue
        l, t, r, b = bbox
        # 水平: ASCII 墨迹居中到左 8 列 (文本模式半宽), 宽字符居中到 16 列
        cx_target = 4 if cp < 0x80 else 8
        ox = int(round((l + r + 1) / 2 - cx_target))
        cell = crop4bit(cov, ox, oy)
        if not any(cell):
            blank += 1
        font_bin += cell
        if (i + 1) % 4000 == 0:
            print(f"      ... {i + 1}/{len(CODES)}")

    cpm_bin = b"".join(struct.pack("<H", c) for c in CODES)
    # 补齐到 256 的整数倍, 保证 font_size == count*128
    total = (len(CODES) + 255) // 256 * 256
    font_bin += bytes((total - len(CODES)) * BYTES_PER_GLYPH)
    cpm_bin += b"\xff\xff" * (total - len(CODES))

    fpath = os.path.join(out_dir, "font.bin")
    cpath = os.path.join(out_dir, "cpm.bin")
    with open(fpath, "wb") as f:
        f.write(font_bin)
    with open(cpath, "wb") as f:
        f.write(cpm_bin)

    print(f"    字模: {fpath} ({len(font_bin)} 字节, {len(CODES)} 字, "
          f"空白 {blank})")
    print(f"    码点: {cpath} ({len(cpm_bin)} 字节)")

    g.DeleteObject(font)
    g.DeleteObject(hb)
    g.DeleteDC(mem)
    return 0


if __name__ == "__main__":
    sys.exit(main())
