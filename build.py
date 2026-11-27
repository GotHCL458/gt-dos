#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
GT-DOS 构建脚本
=======================================================================
  python build.py            # 编译, 生成 build/gt-dos-hd.img
  python build.py run        # 只运行 QEMU (不重新编译)
  python build.py run -nogui # 运行, 只输出到终端(串口), 不开图形窗口
  python build.py run -full  # 运行, 窗口全屏 (分辨率由内核自适配)
  python build.py run -debug # 运行, 打开 CPU/中断跟踪日志
  python build.py all        # 先编译再运行
  python build.py clean      # 清理 build 目录

磁盘布局 (单 HDD 镜像, MBR 分区):
  LBA0        : MBR (boot/mbr.asm, 分区表由本脚本写入)
  part1 C:    : exFAT 系统盘 (含 \GT-DOS\ 系统目录与 INCLUDE 头文件)
  part2 隐藏  : 盘尾启动分区 (type 0x11, bootable):
                [boot.gtb | kernel.gtb | font.gtd | cpm.gtd]
工具链: NASM + clang/LLVM + Rust + QEMU
=======================================================================
"""

import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
QEMU_DIR = os.path.join(ROOT, "qemu")

SECTOR = 512
DISK_SECTORS = 131072               # 64 MB 单硬盘
PART1_LBA = 2048                    # C: 分区起始 (1MB 对齐)
BOOT_PART_SECTORS = 8192            # 启动分区 4MB (4bit 灰度字库 2.75MB)
BOOT_PART_LBA = DISK_SECTORS - BOOT_PART_SECTORS
PART1_SECTORS = BOOT_PART_LBA - PART1_LBA

MAX_KERNEL_SECTORS = 736            # 内核上限 368KB (0x10000..0x6A000)

# --------------------------------------------------------------------- 工具查找
CANDIDATES = {
    "nasm": [
        r"C:\Program Files\NASM\nasm.exe",
        r"C:\Program Files (x86)\NASM\nasm.exe",
        r"D:\NASM\nasm.exe",
        r"C:\msys64\usr\bin\nasm.exe",
    ],
    "clang": [
        r"D:\LLVM\bin\clang.exe",
        r"C:\Program Files\LLVM\bin\clang.exe",
    ],
    "ld.lld": [
        r"D:\LLVM\bin\ld.lld.exe",
        r"C:\Program Files\LLVM\bin\ld.lld.exe",
    ],
    "llvm-objcopy": [
        r"D:\LLVM\bin\llvm-objcopy.exe",
        r"C:\Program Files\LLVM\bin\llvm-objcopy.exe",
    ],
    "cargo": [
        os.path.expanduser(r"~\.cargo\bin\cargo.exe"),
        r"C:\Program Files\Rust\bin\cargo.exe",
    ],
    "qemu-system-i386": [
        os.path.join(QEMU_DIR, "qemu-system-i386.exe"),
        os.path.join(QEMU_DIR, "qemu-system-i386w.exe"),
    ],
    "qemu-system-x86_64": [
        os.path.join(QEMU_DIR, "qemu-system-x86_64.exe"),
        os.path.join(QEMU_DIR, "qemu-system-x86_64w.exe"),
    ],
}

TOOLS = {}


def find_tools():
    missing = []
    for name, paths in CANDIDATES.items():
        found = None
        for p in paths:
            if os.path.isfile(p):
                found = p
                break
        if not found:
            found = shutil.which(name)
        if found:
            TOOLS[name] = found
        elif not name.startswith("qemu"):
            missing.append(name)
    if missing:
        print("[!] 找不到以下工具: " + ", ".join(missing))
        print("    请安装或修改 build.py 中的 CANDIDATES 路径.")
        sys.exit(1)


def _le32(v): return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def host_work_area():
    """宿主桌面可用区域 (不含任务栏). 非 Windows 或获取失败时返回 (0, 0)."""
    if os.name != "nt":
        return 0, 0
    try:
        import ctypes
        from ctypes import wintypes
        rect = wintypes.RECT()
        if not ctypes.windll.user32.SystemParametersInfoW(
                0x0030, 0, ctypes.byref(rect), 0):      # SPI_GETWORKAREA
            return 0, 0
        return rect.right - rect.left, rect.bottom - rect.top
    except Exception:
        return 0, 0


def display_hint(fullscreen=False):
    """窗口模式下给内核的分辨率上限 (宽, 高), 避免 QEMU 窗口被屏幕/任务栏裁切.
    全屏模式由 QEMU 缩放显示, 不做限制, 返回 (0, 0)."""
    if fullscreen:
        return 0, 0
    w, h = host_work_area()
    if w <= 0 or h <= 0:
        return 0, 0
    return max(w - 16, 0), max(h - 48, 0)           # 预留窗口边框与标题栏


# --------------------------------------------------------------------- 构建步骤
def run(cmd, **kw):
    printable = " ".join(cmd)
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(f"\n[!] 命令失败: {printable}")
        if r.stdout:
            print(r.stdout)
        if r.stderr:
            print(r.stderr)
        sys.exit(1)
    if r.stderr.strip():
        print(r.stderr.strip())
    return r


CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-m64",
    "-mcmodel=small",
    "-mno-red-zone",
    "-mno-sse",
    "-mno-sse2",
    "-mno-mmx",
    "-mno-80387",
    "-std=gnu11",
    "-ffreestanding",
    "-fno-builtin",
    "-fstack-protector-strong",
    "-fno-pic",
    "-fno-pie",
    "-fno-asynchronous-unwind-tables",
    "-fno-unwind-tables",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-I", "kernel",
    "-I", os.path.join(ROOT, "build"),
    "-O2",
    "-g",
]

C_SOURCES = [
    "kmain.c",
    "console.c",
    "serial.c",
    "vga.c",
    "tui.c",
    "ui.c",
    "i18n.c",
    "idt.c",
    "softirq.c",
    "apic.c",
    "gfx.c",
    "keyboard.c",
    "mouse.c",
    "timer.c",
    "lib.c",
    "gtlib.c",
    "tc.c",
    "asm.c",
    "sched.c",
    "gui.c",
    "gscreen.c",
    "png.c",
    "ico.c",
    "svg.c",
    "net.c",
    "tcp.c",
    "netapp.c",
    "ata.c",
    "fat.c",
    "cfg.c",
    "user.c",
    "power.c",
    "oobe.c",
    "shell.c",
    "syscall.c",
    "exec.c",
    "task.c",
    "protect.c",
    "richtext.c",
    "vim.c",
]

ASM_SOURCES = ["entry.asm", "isr.asm", "exec_asm.asm", "sched_asm.asm"]

# Rust 组件 (crypto/crc32 内核库 + 宿主工具 gtmkfs/mkfont)
RUST_DIR = os.path.join(ROOT, "rust")
RUST_TARGET = os.path.join(RUST_DIR, "target")
RUST_MANIFEST = os.path.join(RUST_DIR, "Cargo.toml")
RUST_TRIPLE = "x86_64-unknown-none"


def build_rust_tools():
    """编译 Rust 宿主工具: gtmkfs (exFAT 镜像) 与 mkfont (字模)."""
    print("==> 编译 Rust 工具 (gtmkfs / mkfont)")
    run([TOOLS["cargo"], "build", "--release", "--offline",
         "--manifest-path", RUST_MANIFEST])
    exe = ".exe" if os.name == "nt" else ""
    return (os.path.join(RUST_TARGET, "release", "gtmkfs" + exe),
            os.path.join(RUST_TARGET, "release", "mkfont" + exe))


def build_rust_kernel_lib():
    """编译 no_std 的 gtcore 静态库 (AES-128-CBC + CRC32), 链接进内核."""
    print("==> 编译 Rust 内核库 (gtcore)")
    run([TOOLS["cargo"], "build", "--release", "--offline", "-p", "gtcore",
         "--target", RUST_TRIPLE, "--features", "kernel",
         "--manifest-path", RUST_MANIFEST])
    return os.path.join(RUST_TARGET, RUST_TRIPLE, "release", "libgtcore.a")


def ensure_font(mkfont_tool):
    """font.gtd / cpm.gtd 缺失时调用 Rust 版 mkfont 生成."""
    fpath = os.path.join(BUILD, "font.gtd")
    cpath = os.path.join(BUILD, "cpm.gtd")
    if os.path.isfile(fpath) and os.path.isfile(cpath):
        return
    print("==> 字模缺失, 调用 mkfont 生成 (首次较慢)")
    r = subprocess.run([mkfont_tool, BUILD], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    print(r.stdout)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)


def build_boot2():
    src = os.path.join(ROOT, "boot", "boot.asm")
    out = os.path.join(BUILD, "boot.gtb")
    run([TOOLS["nasm"], "-f", "bin", src, "-o", out])
    data = open(out, "rb").read()
    if len(data) != SECTOR * 16:
        print(f"[!] stage2 大小异常: {len(data)} (应为 {SECTOR * 16})")
        sys.exit(1)
    return data


def build_mbr():
    src = os.path.join(ROOT, "boot", "mbr.asm")
    out = os.path.join(BUILD, "mbr.gtb")
    run([TOOLS["nasm"], "-f", "bin", src, "-o", out])
    data = open(out, "rb").read()
    if len(data) != SECTOR:
        print(f"[!] MBR 大小异常: {len(data)}")
        sys.exit(1)
    return data


def build_kernel():
    jobs = []

    for name in ASM_SOURCES:
        src = os.path.join(ROOT, "kernel", name)
        obj = os.path.join(BUILD, name.replace(".asm", ".o"))
        jobs.append(([TOOLS["nasm"], "-f", "elf64", src, "-o", obj], obj))

    for name in C_SOURCES:
        src = os.path.join(ROOT, "kernel", name)
        obj = os.path.join(BUILD, name.replace(".c", ".o"))
        extra = ["-Os"] if name in ("tc.c", "asm.c", "sched.c", "gui.c",
                                    "richtext.c", "png.c", "gscreen.c",
                                    "ico.c", "svg.c") else []
        jobs.append(([TOOLS["clang"]] + CFLAGS + extra + ["-c", src, "-o", obj],
                     obj))

    # 各编译单元相互独立, 并行编译 (结果顺序仍按源列表, 链接稳定)
    def compile_one(job):
        run(job[0], cwd=ROOT)
        return job[1]

    workers = min(len(jobs), os.cpu_count() or 4)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        objs = list(pool.map(compile_one, jobs))

    objs.append(build_rust_kernel_lib())

    elf = os.path.join(BUILD, "kernel.elf")
    run([TOOLS["ld.lld"], "-m", "elf_x86_64", "--gc-sections", "-T",
         os.path.join(ROOT, "kernel", "kernel.ld"),
         "-o", elf] + objs)

    bin_ = os.path.join(BUILD, "kernel.gtb")
    run([TOOLS["llvm-objcopy"], "-O", "binary", elf, bin_])

    data = open(bin_, "rb").read()
    sectors = (len(data) + SECTOR - 1) // SECTOR
    if sectors > MAX_KERNEL_SECTORS:
        print(f"[!] 内核过大: {sectors} 扇区 (上限 {MAX_KERNEL_SECTORS})")
        sys.exit(1)
    print(f"    内核镜像 : {len(data)} 字节 ({sectors} 扇区)")
    return data, sectors


def prepare_kernel_runtime(image):
    """写入运行时只读段校验值.

    .romark 位于只读段边界 (4KB 对齐), 其中 GTR1 结构记录长度与 CRC32;
    KERNEL VERIFY 与空闲期看门狗据此复核代码/只读数据未被改写.
    """
    pos = image.find(b"GTR1")
    if pos < 0 or (pos & 0xFFF) != 0:
        print(f"[!] 内核只读段标记异常 (GTR1@{pos}, 需 4KB 对齐)")
        sys.exit(1)
    crc = zlib.crc32(image[:pos]) & 0xFFFFFFFF
    image = patch_at(image, b"GTR1", pos)
    image = patch_at(image, b"GTR1", crc, skip=4)
    print(f"    只读段   : {pos} 字节 (4KB 页写保护), CRC32={crc:08x}")
    return image


def containerize_kernel(image):
    """把内核镜像封装成受保护容器 (GTK1).

    链接期 .container 节 (48 字节) 收尾, 构建期把其中的 'GTK0' 标记改写为:
      0  'GTK1' | 4 version | 8 image_len | 12 crc32 | 16 sha256[32]
    内核启动时用 Rust gtcore 校验, 任一字节不符即 panic.
    """
    idx = image.find(b"GTK0")
    if idx < 0 or idx + 48 != len(image):
        print(f"[!] 内核容器尾部异常 (GTK0@{idx}, 镜像 {len(image)} 字节)")
        sys.exit(1)

    body = image[:idx]
    crc = zlib.crc32(body) & 0xFFFFFFFF
    digest = hashlib.sha256(body).digest()
    out = body + b"GTK1" + _le32(1) + _le32(idx) + _le32(crc) + digest

    sectors = (len(out) + SECTOR - 1) // SECTOR
    if sectors > MAX_KERNEL_SECTORS:
        print(f"[!] 内核容器过大: {sectors} 扇区 (上限 {MAX_KERNEL_SECTORS})")
        sys.exit(1)
    print(f"    内核容器 : GTK1, {len(out)} 字节 ({sectors} 扇区), "
          f"CRC32={crc:08x}, SHA-256={digest.hex()[:16]}...")
    return out


def patch_at(data, marker, value, skip=0):
    """把 4 字节小端 value 写到 (标记字符串末尾 + skip) 处."""
    idx = data.find(marker)
    if idx < 0:
        print(f"[!] 找不到标记 {marker}")
        sys.exit(1)
    patched = bytearray(data)
    p = idx + len(marker) + skip
    patched[p:p + 4] = _le32(value)
    return bytes(patched)


def build_asm_asset(src_name, out_name):
    """汇编 tools/<src_name> -> build/<out_name>, 返回字节内容."""
    src = os.path.join(ROOT, "tools", src_name)
    out = os.path.join(BUILD, out_name)
    run([TOOLS["nasm"], "-f", "bin", src, "-o", out])
    return open(out, "rb").read()


def build_demo_assets():
    """示例程序 / 共享库 / 数据文件 (随镜像种子写入 C:)."""
    return {
        "HELLO.GTP": build_asm_asset("hello.asm", "hello.gtp"),
        "DEMO.GTP": build_asm_asset("demo.asm", "demo.gtp"),
        "SAMPLE.GTL": build_asm_asset("sample_lib.asm", "sample.gtl"),
        "DATA.GTD": b"GTD-DATA\n",
    }


# ----------------------------------------------------- PNG 测试资源 (构建期生成)
# 纯 Python 生成 PNG, 覆盖全部颜色类型/位深/过滤/Adam7, 供测试与壁纸使用.
# 内核 png.c 完整解码这些文件 (SHOW / PNGINFO / 登录 OOBE 背景).

def _png_chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


def _png_filter(ftype, row, prev, bpp):
    n = len(row)
    if ftype == 0:
        return bytes(row)
    out = bytearray(n)
    if ftype == 1:
        for i in range(n):
            a = row[i - bpp] if i >= bpp else 0
            out[i] = (row[i] - a) & 0xFF
    elif ftype == 2:
        for i in range(n):
            b = prev[i] if prev else 0
            out[i] = (row[i] - b) & 0xFF
    elif ftype == 3:
        for i in range(n):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i] if prev else 0
            out[i] = (row[i] - ((a + b) >> 1)) & 0xFF
    else:                                    # 4 = Paeth
        for i in range(n):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i] if prev else 0
            c = prev[i - bpp] if (prev and i >= bpp) else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            out[i] = (row[i] - pr) & 0xFF
    return bytes(out)


def _png_encode(w, h, ctype, depth, full_rows, px_bytes, palette=None,
                trns=None, interlace=0, split=0, filt=None):
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)
    chunks = [_png_chunk(b"IHDR", ihdr)]
    if palette is not None:
        chunks.append(_png_chunk(b"PLTE", palette))
    if trns is not None:
        chunks.append(_png_chunk(b"tRNS", trns))

    scan = bytearray()
    if interlace == 0:
        prev = None
        for y in range(h):
            row = full_rows[y]
            f = filt(y) if filt else (y % 5)
            scan += bytes([f]) + _png_filter(f, row, prev, px_bytes)
            prev = row
    else:
        xs = (0, 4, 0, 2, 0, 1, 0)
        ys = (0, 0, 4, 0, 2, 0, 1)
        dxs = (8, 8, 4, 4, 2, 2, 1)
        dys = (8, 8, 8, 4, 4, 2, 2)
        for p in range(7):
            x0, y0, dx, dy = xs[p], ys[p], dxs[p], dys[p]
            pw = (w - x0 + dx - 1) // dx
            ph = (h - y0 + dy - 1) // dy
            if pw <= 0 or ph <= 0:
                continue
            prev = None
            for r in range(ph):
                srow = full_rows[y0 + r * dy]
                brow = bytearray()
                for c in range(pw):
                    sx = x0 + c * dx
                    brow += srow[sx * px_bytes:(sx + 1) * px_bytes]
                f = r % 5
                brow = bytes(brow)
                scan += bytes([f]) + _png_filter(f, brow, prev, px_bytes)
                prev = brow

    comp = zlib.compress(bytes(scan), 9)
    if split and split > 0:
        for i in range(0, len(comp), split):
            chunks.append(_png_chunk(b"IDAT", comp[i:i + split]))
    else:
        chunks.append(_png_chunk(b"IDAT", comp))
    chunks.append(_png_chunk(b"IEND", b""))
    return b"\x89PNG\r\n\x1a\n" + b"".join(chunks)


def build_png_assets():
    """生成各颜色类型/位深/过滤/隔行的 PNG + 壁纸."""
    out = {}

    # SAMPLE.PNG: RGBA8 128x128, 透明角 + 中心标记, IDAT 分块 (测试串联读取)
    W = H = 128
    rows = []
    for y in range(H):
        row = bytearray(W * 4)
        for x in range(W):
            r = x * 255 // (W - 1)
            g = y * 255 // (H - 1)
            b = 128
            a = 255
            if x < 8 and y < 8:
                a = 0
            if x == W // 2 and y == H // 2:
                r, g, b, a = 255, 32, 200, 255
            o = x * 4
            row[o:o + 4] = bytes((r, g, b, a))
        rows.append(bytes(row))
    out["SAMPLE.PNG"] = _png_encode(W, H, 6, 8, rows, 4, split=512)

    # RGB8.PNG: 64x64, 每行依次使用过滤 0..4
    W = H = 64
    rows = []
    for y in range(H):
        row = bytearray(W * 3)
        for x in range(W):
            r = (20 + x * 3) & 255
            g = (30 + y * 3) & 255
            b = (200 - x) & 255
            if x == W // 2 and y == H // 2:
                r, g, b = 10, 200, 30
            o = x * 3
            row[o:o + 3] = bytes((r, g, b))
        rows.append(bytes(row))
    out["RGB8.PNG"] = _png_encode(W, H, 2, 8, rows, 3, split=256)

    # PAL8.PNG: 32x32 调色板 8 位 + tRNS (index0 透明)
    W = H = 32
    palette = bytes((255, 0, 0, 0, 128, 255, 0, 255, 0, 30, 30, 30))
    trns = bytes((0, 255, 255, 255))
    rows = []
    for y in range(H):
        row = bytearray(W)
        for x in range(W):
            row[x] = 0 if (x < 8 and y < 8) else 1
        rows.append(bytes(row))
    out["PAL8.PNG"] = _png_encode(W, H, 3, 8, rows, 1,
                                  palette=palette, trns=trns)

    # GRAY.PNG: 32x32 灰度 8 位
    W = H = 32
    rows = []
    for y in range(H):
        row = bytearray(W)
        for x in range(W):
            v = (x * 8 + y * 4) & 255
            if x == W // 2 and y == H // 2:
                v = 200
            row[x] = v
        rows.append(bytes(row))
    out["GRAY.PNG"] = _png_encode(W, H, 0, 8, rows, 1)

    # GA.PNG: 32x32 灰度 + alpha
    W = H = 32
    rows = []
    for y in range(H):
        row = bytearray(W * 2)
        for x in range(W):
            v = (x * 6 + y * 3) & 255
            a = 255
            if x == W // 2 and y == H // 2:
                v, a = 200, 255
            if x < 4 and y < 4:
                a = 0
            o = x * 2
            row[o] = v
            row[o + 1] = a
        rows.append(bytes(row))
    out["GA.PNG"] = _png_encode(W, H, 4, 8, rows, 2)

    # RGB16.PNG: 16x16 真彩 16 位
    W = H = 16
    rows = []
    for y in range(H):
        row = bytearray(W * 6)
        for x in range(W):
            r = (x * 4000) & 0xFFFF
            g = (y * 4000) & 0xFFFF
            b = 30000
            if x == W // 2 and y == H // 2:
                r, g, b = 1000, 50000, 30000
            o = x * 6
            row[o:o + 2] = struct.pack(">H", r)
            row[o + 2:o + 4] = struct.pack(">H", g)
            row[o + 4:o + 6] = struct.pack(">H", b)
        rows.append(bytes(row))
    out["RGB16.PNG"] = _png_encode(W, H, 2, 16, rows, 6)

    # PAL1.PNG: 16x16 调色板 1 位
    W = H = 16
    palette = bytes((10, 20, 30, 200, 100, 50))
    rows = []
    for y in range(H):
        row = bytearray(W // 8)
        for x in range(W):
            if x >= W // 2:
                row[x // 8] |= (1 << (7 - (x % 8)))
        rows.append(bytes(row))
    out["PAL1.PNG"] = _png_encode(W, H, 3, 1, rows, 1, palette=palette)

    # ADAM7.PNG: 64x64 RGBA8 隔行
    W = H = 64
    rows = []
    for y in range(H):
        row = bytearray(W * 4)
        for x in range(W):
            r = (x * 4) & 255
            g = (y * 4) & 255
            b = (x + y) & 255
            a = 255
            if x == W // 2 and y == H // 2:
                r, g, b, a = 250, 10, 120, 255
            o = x * 4
            row[o:o + 4] = bytes((r, g, b, a))
        rows.append(bytes(row))
    out["ADAM7.PNG"] = _png_encode(W, H, 6, 8, rows, 4, interlace=1)

    # WALLPAPER.PNG: 256x192 RGBA8 竖向渐变 (登录/OOBE 背景)
    W, H = 256, 192
    rows = []
    for y in range(H):
        row = bytearray(W * 4)
        t = y * 255 // (H - 1)
        r = (14 * (255 - t) + 47 * t) // 255
        g = (26 * (255 - t) + 90 * t) // 255
        b = (48 * (255 - t) + 122 * t) // 255
        for x in range(W):
            o = x * 4
            row[o:o + 4] = bytes((r, g, b, 255))
        rows.append(bytes(row))
    out["WALLPAPER.PNG"] = _png_encode(W, H, 6, 8, rows, 4,
                                       filt=lambda y: 0, split=8192)
    return out


# ------------------------------------------------- SVG -> ICO 系统图标
# 图标以 SVG 描述 (纯填充图形), 构建期用 PIL 4x 超采样光栅化为 128x128 RGBA,
# 再封装成 32bpp BMP 的 ICO. 内核 ico.c 加载并缓存, 桌面/面板/菜单使用.
# PIL 不可用时返回空字典, 内核回退到内置矢量图标.

ICON_SVGS = {
    "TERM": """<svg viewBox="0 0 128 128">
  <rect x="14" y="20" width="100" height="88" fill="#0f1720"/>
  <rect x="14" y="20" width="100" height="22" fill="#2f6f9f"/>
  <circle cx="28" cy="31" r="4" fill="#e05a5a"/>
  <circle cx="42" cy="31" r="4" fill="#e0c05a"/>
  <circle cx="56" cy="31" r="4" fill="#5ae08a"/>
  <path d="M32 58 L54 74 L32 90" fill="#7fe0a0"/>
  <rect x="62" y="84" width="34" height="8" fill="#7fe0a0"/>
</svg>""",
    "FILES": """<svg viewBox="0 0 128 128">
  <rect x="16" y="28" width="52" height="22" fill="#c9a227"/>
  <rect x="16" y="44" width="96" height="64" fill="#e6c34a"/>
  <rect x="16" y="44" width="96" height="8" fill="#f3d873"/>
  <rect x="30" y="66" width="68" height="6" fill="#a8861d"/>
  <rect x="30" y="80" width="52" height="6" fill="#a8861d"/>
</svg>""",
    "SYS": """<svg viewBox="0 0 128 128">
  <rect x="30" y="30" width="68" height="68" fill="#2f7f7f"/>
  <rect x="44" y="44" width="40" height="40" fill="#102028"/>
  <rect x="52" y="52" width="24" height="24" fill="#5fc0c0"/>
  <rect x="38" y="20" width="6" height="10" fill="#c0e0e0"/>
  <rect x="60" y="20" width="6" height="10" fill="#c0e0e0"/>
  <rect x="84" y="20" width="6" height="10" fill="#c0e0e0"/>
  <rect x="38" y="98" width="6" height="10" fill="#c0e0e0"/>
  <rect x="60" y="98" width="6" height="10" fill="#c0e0e0"/>
  <rect x="84" y="98" width="6" height="10" fill="#c0e0e0"/>
  <rect x="20" y="38" width="10" height="6" fill="#c0e0e0"/>
  <rect x="20" y="60" width="10" height="6" fill="#c0e0e0"/>
  <rect x="20" y="84" width="10" height="6" fill="#c0e0e0"/>
  <rect x="98" y="38" width="10" height="6" fill="#c0e0e0"/>
  <rect x="98" y="60" width="10" height="6" fill="#c0e0e0"/>
  <rect x="98" y="84" width="10" height="6" fill="#c0e0e0"/>
</svg>""",
    "SETTINGS": """<svg viewBox="0 0 128 128">
  <rect x="16" y="34" width="96" height="8" fill="#3daee9"/>
  <circle cx="46" cy="38" r="11" fill="#eff0f1"/>
  <rect x="16" y="62" width="96" height="8" fill="#3daee9"/>
  <circle cx="82" cy="66" r="11" fill="#eff0f1"/>
  <rect x="16" y="90" width="96" height="8" fill="#3daee9"/>
  <circle cx="58" cy="94" r="11" fill="#eff0f1"/>
</svg>""",
    "ABOUT": """<svg viewBox="0 0 128 128">
  <circle cx="64" cy="64" r="46" fill="#3daee9"/>
  <rect x="58" y="54" width="12" height="42" fill="#ffffff"/>
  <circle cx="64" cy="40" r="8" fill="#ffffff"/>
</svg>""",
    "WIDGETS": """<svg viewBox="0 0 128 128">
  <rect x="22" y="22" width="38" height="38" fill="#3daee9"/>
  <rect x="68" y="22" width="38" height="38" fill="#7fe0a0"/>
  <rect x="22" y="68" width="38" height="38" fill="#ffc060"/>
  <rect x="68" y="68" width="38" height="38" fill="#c090ff"/>
</svg>""",
    "START": """<svg viewBox="0 0 128 128">
  <rect x="12" y="12" width="104" height="104" fill="#1b2430"/>
  <circle cx="40" cy="40" r="9" fill="#3daee9"/>
  <circle cx="64" cy="40" r="9" fill="#7fe0a0"/>
  <circle cx="88" cy="40" r="9" fill="#ffc060"/>
  <circle cx="40" cy="64" r="9" fill="#7fe0a0"/>
  <circle cx="64" cy="64" r="9" fill="#ffc060"/>
  <circle cx="88" cy="64" r="9" fill="#c090ff"/>
  <circle cx="40" cy="88" r="9" fill="#ffc060"/>
  <circle cx="64" cy="88" r="9" fill="#c090ff"/>
  <circle cx="88" cy="88" r="9" fill="#3daee9"/>
</svg>""",
}


def _svg_attrs(s):
    d = {}
    for m in re.finditer(r'([\w:-]+)\s*=\s*"([^"]*)"', s):
        d[m.group(1)] = m.group(2)
    return d


def _svg_color(v, default=(0, 0, 0, 255)):
    if not v:
        return default
    v = v.strip().lower()
    names = {"black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0),
             "green": (0, 128, 0), "lime": (0, 255, 0), "blue": (0, 0, 255),
             "yellow": (255, 255, 0), "orange": (255, 165, 0),
             "gray": (128, 128, 128), "grey": (128, 128, 128),
             "purple": (128, 0, 128), "cyan": (0, 255, 255)}
    if v in names:
        return names[v] + (255,)
    if v.startswith("#"):
        h = v[1:]
        try:
            if len(h) == 3:
                return (int(h[0] * 2, 16), int(h[1] * 2, 16),
                        int(h[2] * 2, 16), 255)
            if len(h) == 6:
                return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), 255)
            if len(h) == 8:
                return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16),
                        int(h[6:8], 16))
        except ValueError:
            pass
    return default


def _svg_path_polys(d):
    toks = re.findall(r'[MLHVQCZmlhvqcz]|-?\d*\.?\d+', d)
    i = 0
    cmd = None
    cx = cy = sx = sy = 0.0
    pts = []
    polys = []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            i += 1
            if cmd in "Zz":
                if len(pts) > 2:
                    polys.append(pts)
                pts = []
                cx, cy = sx, sy
            continue
        if cmd is None:
            i += 1
            continue
        if cmd in "Mm":
            x = num(); y = num()
            if cmd == "m":
                x += cx; y += cy
            if len(pts) > 2:
                polys.append(pts)
            pts = [(x, y)]
            cx, cy = sx, sy = x, y
            cmd = "L" if cmd == "M" else "l"
        elif cmd in "Ll":
            x = num(); y = num()
            if cmd == "l":
                x += cx; y += cy
            pts.append((x, y)); cx, cy = x, y
        elif cmd in "Hh":
            x = num()
            if cmd == "h":
                x += cx
            cx = x; pts.append((cx, cy))
        elif cmd in "Vv":
            y = num()
            if cmd == "v":
                y += cy
            cy = y; pts.append((cx, cy))
        elif cmd in "Qq":
            x1 = num(); y1 = num(); x = num(); y = num()
            if cmd == "q":
                x1 += cx; y1 += cy; x += cx; y += cy
            for k in range(1, 17):
                tt = k / 16.0; u = 1 - tt
                pts.append((u * u * cx + 2 * u * tt * x1 + tt * tt * x,
                            u * u * cy + 2 * u * tt * y1 + tt * tt * y))
            cx, cy = x, y
        elif cmd in "Cc":
            x1 = num(); y1 = num(); x2 = num(); y2 = num()
            x = num(); y = num()
            if cmd == "c":
                x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy
            for k in range(1, 17):
                tt = k / 16.0; u = 1 - tt
                pts.append((u ** 3 * cx + 3 * u * u * tt * x1 +
                            3 * u * tt * tt * x2 + tt ** 3 * x,
                            u ** 3 * cy + 3 * u * u * tt * y1 +
                            3 * u * tt * tt * y2 + tt ** 3 * y))
            cx, cy = x, y
        else:
            i += 1
    if len(pts) > 2:
        polys.append(pts)
    return polys


def _svg_render(svg, size=128, ss=4):
    from PIL import Image, ImageDraw
    vb = re.search(r'viewBox\s*=\s*"([^"]*)"', svg)
    if vb:
        nums = [float(x) for x in re.findall(r'-?\d*\.?\d+', vb.group(1))]
        minx, miny, vw, vh = (nums + [0, 0, 128, 128])[:4]
    else:
        minx = miny = 0.0
        vw = vh = 128.0
    if vw <= 0 or vh <= 0:
        vw = vh = 128.0
    W = size * ss
    sc = min(W / vw, W / vh)
    ox = (W - vw * sc) / 2.0 - minx * sc
    oy = (W - vh * sc) / 2.0 - miny * sc

    def mx(x):
        return ox + x * sc

    def my(y):
        return oy + y * sc

    base = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    for m in re.finditer(r'<(rect|circle|ellipse|path)\b([^>]*)>', svg):
        kind = m.group(1)
        a = _svg_attrs(m.group(2))
        col = _svg_color(a.get("fill"))
        op = float(a.get("opacity", "1") or 1)
        fop = float(a.get("fill-opacity", "1") or 1)
        alpha = max(0, min(255, int(col[3] * op * fop)))
        if alpha == 0:
            continue
        layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        dr = ImageDraw.Draw(layer)
        fill = (col[0], col[1], col[2], alpha)
        if kind == "rect":
            x = float(a.get("x", 0)); y = float(a.get("y", 0))
            w = float(a.get("width", 0)); h = float(a.get("height", 0))
            if w > 0 and h > 0:
                dr.rectangle([mx(x), my(y), mx(x + w) - 1, my(y + h) - 1],
                             fill=fill)
        elif kind in ("circle", "ellipse"):
            cx = float(a.get("cx", 0)); cy = float(a.get("cy", 0))
            if kind == "circle":
                rx = ry = float(a.get("r", 0))
            else:
                rx = float(a.get("rx", 0)); ry = float(a.get("ry", 0))
            if rx > 0 and ry > 0:
                dr.ellipse([mx(cx - rx), my(cy - ry),
                            mx(cx + rx) - 1, my(cy + ry) - 1], fill=fill)
        elif kind == "path":
            for poly in _svg_path_polys(a.get("d", "")):
                dr.polygon([(mx(px), my(py)) for px, py in poly], fill=fill)
        base = Image.alpha_composite(base, layer)
    return base.resize((size, size), Image.LANCZOS)


def _rgba_to_ico(img):
    """RGBA PIL 图像 -> 32bpp BMP 单条目 ICO (内核直接支持)."""
    w, h = img.size
    rgba = img.tobytes()
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        row = rgba[y * w * 4:(y + 1) * w * 4]
        for x in range(w):
            r, g, b, a = row[x * 4:x * 4 + 4]
            xor += bytes((b, g, r, a))
    maskrow = ((w + 31) // 32) * 4
    mask = bytes(maskrow * h)
    dib = (struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                       len(xor) + len(mask), 0, 0, 0, 0) + bytes(xor) + mask)
    icondir = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", w if w < 256 else 0, h if h < 256 else 0,
                        0, 0, 1, 32, len(dib), 6 + 16)
    return icondir + entry + dib


def build_icon_assets():
    """SVG 图标 -> 高质量 ICO; 同时带一份 SVG 源文件供 SHOW 查看."""
    out = {}
    try:
        from PIL import Image  # noqa: F401
    except Exception:
        return out
    for name, svg in ICON_SVGS.items():
        try:
            img = _svg_render(svg, 64, 4)
            out[name + ".ICO"] = _rgba_to_ico(img)
            out[name + ".SVG"] = svg.encode("utf-8")
        except Exception:
            continue
    return out



# ------------------------------------------------------- 构建期 exFAT 工具 (Rust)
# 卷格式化、系统文件加密、目录项与厂商扩展全部由 rust/gtmkfs 完成;
# 本脚本只生成清单并调用一次该工具, 不参与任何加密或文件系统逻辑。

def write_manifest(assets, services, kernel_image=None):
    """生成种子清单: 系统头文件/服务/内核镜像加密, 示例程序/库/数据保持明文."""
    inc_dir = os.path.join(ROOT, "kernel", "include")
    lines = ["D \\gt-dos 0x10 SYSTEM", "D \\gt-dos\\include 0x10 SYSTEM"]
    for fn in sorted(os.listdir(inc_dir)):
        if fn.endswith(".h"):
            host = os.path.join(inc_dir, fn)
            lines.append(f"F \\gt-dos\\include\\{fn.lower()} {host} 0x25 SYSTEM 1")
    for name, payload in assets.items():
        host = os.path.join(BUILD, name.lower())
        with open(host, "wb") as f:
            f.write(payload)
        lines.append(f"F \\{name.lower()} {host} 0x20 SYSTEM 0")

    # 系统服务: 加密的 .gtp 程序, 以 SYS 任务身份运行且受保护
    lines.append("D \\gt-dos\\services 0x10 SYSTEM")
    for name, payload in services.items():
        host = os.path.join(BUILD, name.lower())
        with open(host, "wb") as f:
            f.write(payload)
        lines.append(f"F \\gt-dos\\services\\{name.lower()} {host} 0x21 SYSTEM 1")

    # 内核镜像的受保护 .gtp 视图: 与 boot 载入的容器内容一致, 加密存放.
    # 不参与执行 (exec 拒绝), 供 KERNEL 命令核对.
    if kernel_image is not None:
        host = os.path.join(BUILD, "kernel.gtp")
        with open(host, "wb") as f:
            f.write(kernel_image)
        lines.append(f"F \\gt-dos\\kernel.gtp {host} 0x21 SYSTEM 1")

    # 系统内 SDK: 明文只读, 供 TC 编译示例程序
    sdk_dir = os.path.join(ROOT, "tools", "sdk")
    if os.path.isdir(sdk_dir):
        lines.append("D \\sdk 0x10 SYSTEM")
        for fn in sorted(os.listdir(sdk_dir)):
            host = os.path.join(sdk_dir, fn)
            if os.path.isfile(host):
                lines.append(f"F \\sdk\\{fn.lower()} {host} 0x21 SYSTEM 0")

    mf = os.path.join(BUILD, "mkfs.txt")
    with open(mf, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    return mf


def make_hd_image(mbr, boot2, kernel, font, cpm, assets, services, mkfs_tool,
                  kernel_image=None):
    """组装单 HDD 镜像: MBR + C: exFAT + 盘尾启动分区."""
    boot_secs = 16
    kern_secs = (len(kernel) + SECTOR - 1) // SECTOR
    font_secs = (len(font) + SECTOR - 1) // SECTOR
    cpm_secs = (len(cpm) + SECTOR - 1) // SECTOR
    used = boot_secs + kern_secs + font_secs + cpm_secs
    if used > BOOT_PART_SECTORS:
        print(f"[!] 启动分区放不下: 需要 {used} 扇区, 只有 {BOOT_PART_SECTORS}")
        sys.exit(1)

    # 分区内偏移 -> 绝对 LBA
    k_lba = BOOT_PART_LBA + boot_secs
    f_lba = k_lba + kern_secs
    c_lba = f_lba + font_secs

    # 补丁 stage2
    boot2 = patch_at(boot2, b"GTCN", kern_secs)
    boot2 = patch_at(boot2, b"GTKL", k_lba)
    boot2 = patch_at(boot2, b"GTFL", f_lba)
    boot2 = patch_at(boot2, b"GTFS", font_secs)
    boot2 = patch_at(boot2, b"GTCL", c_lba)
    boot2 = patch_at(boot2, b"GTCS", cpm_secs)

    # 补丁 MBR: 启动分区起始 LBA (标记末尾 +8 = DAP 的 LBA low 字段)
    mbr = patch_at(mbr, b"GTD0", BOOT_PART_LBA, skip=8)

    # 启动分区 raw 内容
    bootpart = bytearray(BOOT_PART_SECTORS * SECTOR)
    def put(off_sect, data):
        bootpart[off_sect * SECTOR:off_sect * SECTOR + len(data)] = data
    put(0, boot2)
    put(boot_secs, kernel)
    put(boot_secs + kern_secs, font)
    put(boot_secs + kern_secs + font_secs, cpm)

    # 分区表
    def pe(bootable, ptype, lba, count):
        e = bytearray(16)
        e[0] = bootable
        e[1:4] = b"\x00\x02\x01"
        e[4] = ptype
        e[5:8] = b"\xFF\xFE\xFF"
        e[8:12] = _le32(lba)
        e[12:16] = _le32(count)
        return e

    mbr = bytearray(mbr)
    mbr[0x1BE:0x1CE] = pe(0x00, 0x07, PART1_LBA, PART1_SECTORS)
    mbr[0x1CE:0x1DE] = pe(0x80, 0x11, BOOT_PART_LBA, BOOT_PART_SECTORS)

    img = bytearray(DISK_SECTORS * SECTOR)
    img[0:SECTOR] = mbr
    img[BOOT_PART_LBA * SECTOR:] = bootpart

    path = os.path.join(BUILD, "gt-dos-hd.img")
    with open(path, "wb") as f:
        f.write(img)

    mf = write_manifest(assets, services, kernel_image)
    r = subprocess.run([mkfs_tool, path, str(PART1_LBA), str(PART1_SECTORS), mf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        print("[!] exFAT 格式化失败")
        sys.exit(1)
    if r.stdout.strip():
        print(r.stdout.rstrip())

    print(f"    硬盘镜像 : {path} ({DISK_SECTORS * SECTOR} 字节)")
    print(f"    C: 分区  : LBA {PART1_LBA} + {PART1_SECTORS} 扇区 (exFAT)")
    print(f"    启动分区 : LBA {BOOT_PART_LBA} + {BOOT_PART_SECTORS} 扇区 "
          f"(boot16/kern{kern_secs}/font{font_secs}/cpm{cpm_secs})")
    return path


TC_LIB_MODULES = [
    ("string", "string.c"),
    ("stdlib", "stdlib.c"),
    ("ctype", "ctype.c"),
    ("stdio", "stdio.c"),
    ("net", "net.c"),
    ("gui", "gui.c"),
]


def gen_tc_lib():
    """把按模块拆分的 tools/sdk/*.c 转成 C 字符串头.

    TC 在源文件 #include 对应头文件后, 才把该模块编入程序."""
    parts = ["/* 自动生成: tools/sdk 下各模块 -> TC 按需编入的标准库源码 */",
             "#ifndef GT_TC_LIB_H", "#define GT_TC_LIB_H"]
    total = 0
    for mod, fn in TC_LIB_MODULES:
        path = os.path.join(ROOT, "tools", "sdk", fn)
        if not os.path.isfile(path):
            print(f"[!] 缺少 tools/sdk/{fn}")
            sys.exit(1)
        text = open(path, encoding="utf-8").read()
        total += len(text)
        lines = []
        for line in text.splitlines():
            esc = line.replace("\\", "\\\\").replace('"', '\\"')
            lines.append('    "%s\\n"' % esc)
        parts.append(f"static const char tc_lib_{mod}[] =\n" +
                     "\n".join(lines) + ";")
    parts.append("#endif")
    with open(os.path.join(BUILD, "tc_lib.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts) + "\n")
    print(f"==> 生成 TC 标准库 ({total} 字节, {len(TC_LIB_MODULES)} 个模块)")


def build():
    os.makedirs(BUILD, exist_ok=True)
    gen_tc_lib()
    mkfs_tool, mkfont_tool = build_rust_tools()
    print("==> 编译 MBR / stage2 (NASM, 16-bit)")
    mbr = build_mbr()
    boot2 = build_boot2()
    print("==> 编译内核 (NASM + clang/LLVM + Rust, x86-64 long mode)")
    kernel, _ = build_kernel()
    hint_w, hint_h = display_hint()
    if hint_w and hint_h:
        # 让内核知道宿主可用区域, 窗口模式选不会超出桌面的分辨率
        kernel = patch_at(kernel, b"GTMH", hint_w)
        kernel = patch_at(kernel, b"GTMH", hint_h, skip=4)
    # 运行时防篡改: 只读段校验值 + 受保护容器尾部 (所有补丁完成后计算)
    kernel = prepare_kernel_runtime(kernel)
    kernel = containerize_kernel(kernel)
    ensure_font(mkfont_tool)
    font = open(os.path.join(BUILD, "font.gtd"), "rb").read()
    cpm = open(os.path.join(BUILD, "cpm.gtd"), "rb").read()
    assets = build_demo_assets()
    assets.update(build_png_assets())
    assets.update(build_icon_assets())
    services = {"NETSVC.GTP": build_asm_asset("svc_net.asm", "svc_net.gtp")}
    print("==> 组装单硬盘镜像 (MBR + C: exFAT + 隐藏启动分区)")
    hd = make_hd_image(mbr, boot2, kernel, font, cpm, assets, services,
                       mkfs_tool, kernel_image=kernel)
    return hd


# --------------------------------------------------------------------- 运行
def set_utf8_console():
    """把 Windows 控制台输出码页切到 UTF-8, 否则 QEMU 串口输出的
    UTF-8 中文字节会被 GBK 码页解码成乱码."""
    if os.name != "nt":
        return
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        k32.SetConsoleOutputCP(65001)
        k32.SetConsoleCP(65001)
    except Exception:
        pass


def patch_image_display_hint(hd, w, h):
    """就地改写镜像启动分区中内核的显示区域提示 (w=h=0 表示不限制)."""
    base = (BOOT_PART_LBA + 16) * SECTOR     # 启动分区: boot2(16 扇区) + kernel
    try:
        with open(hd, "rb") as f:
            f.seek(base)
            blob = f.read(MAX_KERNEL_SECTORS * SECTOR)
        idx = blob.find(b"GTMH")
        if idx < 0:
            return
        with open(hd, "r+b") as f:
            f.seek(base + idx + 4)
            f.write(_le32(w) + _le32(h))
    except OSError:
        pass


def run_qemu(hd, nogui=False, debug=False, full=False):
    if "qemu-system-x86_64" not in TOOLS:
        print("[!] 找不到 qemu-system-x86_64.exe")
        sys.exit(1)
    set_utf8_console()

    # 按本次运行方式重写显示区域提示: 全屏不限制, 窗口模式限制在桌面可用区域内
    if not nogui:
        patch_image_display_hint(hd, *display_hint(full))

    cmd = [
        TOOLS["qemu-system-x86_64"],
        "-m", "32",
        "-hda", hd,
        "-boot", "c",
        "-monitor", "none",
        "-serial", "stdio",
        "-rtc", "base=localtime",
        "-netdev", "user,id=n0",
        "-device", "rtl8139,netdev=n0",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    if nogui:
        cmd += ["-display", "none"]
    else:
        # 鼠标移入窗口即捕获, 并隐藏宿主指针: 避免宿主光标与来宾指针重叠闪烁,
        # 也避免移到屏幕边缘时指针逃出窗口 (Ctrl+Alt+G 释放)
        cmd += ["-display", "gtk,grab-on-hover=on,show-cursor=off"]
    if full:
        cmd += ["-full-screen"]
    if debug:
        cmd += ["-d", "int,cpu_reset", "-no-reboot", "-no-shutdown"]

    print("\n==> 启动 QEMU:")
    print("    " + " ".join(cmd))
    print("    串口输出直接显示在本终端; 引导时先出现 BIOS 信息属正常现象.")
    print("    图形桌面: 输入 DESKTOP; 鼠标移入窗口自动捕获, Ctrl+Alt+G 释放.\n")
    try:
        subprocess.call(cmd)
    except KeyboardInterrupt:
        pass


def clean():
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
        print("已清理 build 目录")


def image_path():
    return os.path.join(BUILD, "gt-dos-hd.img")


def main():
    args = [a.lower() for a in sys.argv[1:]]
    if "clean" in args:
        clean()
        return

    do_build = "run" not in args or "build" in args or "all" in args
    do_run = "run" in args or "all" in args

    if do_build:
        t0 = time.time()
        find_tools()
        build()
        print(f"==> 构建完成, 耗时 {time.time() - t0:.2f}s")

    if do_run:
        hd = image_path()
        if not os.path.isfile(hd):
            print("[!] 未找到镜像, 请先运行: python build.py")
            sys.exit(1)
        find_tools()
        run_qemu(hd,
                 nogui=("-nogui" in args or "--nogui" in args),
                 debug=("-debug" in args or "--debug" in args),
                 full=("-full" in args or "--full" in args))


if __name__ == "__main__":
    main()
