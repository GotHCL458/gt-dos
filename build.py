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
工具链: NASM + clang/LLVM + QEMU
=======================================================================
"""

import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, "build")
QEMU_DIR = os.path.join(ROOT, "qemu")

SECTOR = 512
DISK_SECTORS = 131072               # 64 MB 单硬盘
PART1_LBA = 2048                    # C: 分区起始 (1MB 对齐)
BOOT_PART_SECTORS = 8192            # 启动分区 4MB (4bit 灰度字库 2.75MB)
BOOT_PART_LBA = DISK_SECTORS - BOOT_PART_SECTORS
PART1_SECTORS = BOOT_PART_LBA - PART1_LBA

MAX_KERNEL_SECTORS = 512            # 内核上限 256KB (受 0x10000..0x80000 限制)

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
    "crypto.c",
    "gtlib.c",
    "tc.c",
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
]

ASM_SOURCES = ["entry.asm", "isr.asm", "exec_asm.asm"]


def ensure_font():
    """font.gtd / cpm.gtd 缺失时自动调用 tools/mkfont.py 生成."""
    fpath = os.path.join(BUILD, "font.gtd")
    cpath = os.path.join(BUILD, "cpm.gtd")
    if os.path.isfile(fpath) and os.path.isfile(cpath):
        return
    print("==> 字模缺失, 调用 tools/mkfont.py 生成 (首次较慢)")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfont.py")],
                       capture_output=True, text=True)
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
    objs = []

    for name in ASM_SOURCES:
        src = os.path.join(ROOT, "kernel", name)
        obj = os.path.join(BUILD, name.replace(".asm", ".o"))
        run([TOOLS["nasm"], "-f", "elf64", src, "-o", obj])
        objs.append(obj)

    for name in C_SOURCES:
        src = os.path.join(ROOT, "kernel", name)
        obj = os.path.join(BUILD, name.replace(".c", ".o"))
        run([TOOLS["clang"]] + CFLAGS + ["-c", src, "-o", obj], cwd=ROOT)
        objs.append(obj)

    elf = os.path.join(BUILD, "kernel.elf")
    run([TOOLS["ld.lld"], "-m", "elf_x86_64", "-T",
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


# ------------------------------------------------------- 构建期 exFAT 工具 (纯 C)
# 卷格式化、系统文件加密、目录项与厂商扩展全部由 tools/gtmkfs.c 完成;
# 本脚本只生成清单并调用一次该工具, 不参与任何加密或文件系统逻辑.

def _host_cc():
    for c in (os.path.join(ROOT, "mingw64", "bin", "gcc.exe"),
              r"C:\mingw64\bin\gcc.exe",
              r"C:\Program Files\mingw64\bin\gcc.exe",
              "gcc"):
        if os.path.isabs(c):
            if os.path.isfile(c):
                return c
        else:
            return c
    return "gcc"


def build_mkfs_tool():
    """编译独立 exFAT 工具 build/gtmkfs.exe (复用 kernel/crypto.c)."""
    out = os.path.join(BUILD, "gtmkfs.exe")
    run([_host_cc(), "-O2", "-I", os.path.join(ROOT, "kernel"),
         os.path.join(ROOT, "tools", "gtmkfs.c"),
         os.path.join(ROOT, "kernel", "crypto.c"), "-o", out])
    return out


def write_manifest(assets):
    """生成种子清单: 系统头文件加密, 示例程序/库/数据保持明文."""
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


def make_hd_image(mbr, boot2, kernel, font, cpm, assets, mkfs_tool):
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

    mf = write_manifest(assets)
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


def build():
    os.makedirs(BUILD, exist_ok=True)
    print("==> 编译 MBR / stage2 (NASM, 16-bit)")
    mbr = build_mbr()
    boot2 = build_boot2()
    print("==> 编译内核 (NASM + clang/LLVM, x86-64 long mode)")
    kernel, _ = build_kernel()
    ensure_font()
    font = open(os.path.join(BUILD, "font.gtd"), "rb").read()
    cpm = open(os.path.join(BUILD, "cpm.gtd"), "rb").read()
    assets = build_demo_assets()
    mkfs_tool = build_mkfs_tool()
    print("==> 组装单硬盘镜像 (MBR + C: exFAT + 隐藏启动分区)")
    hd = make_hd_image(mbr, boot2, kernel, font, cpm, assets, mkfs_tool)
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


def run_qemu(hd, nogui=False, debug=False, full=False):
    if "qemu-system-x86_64" not in TOOLS:
        print("[!] 找不到 qemu-system-x86_64.exe")
        sys.exit(1)
    set_utf8_console()

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
    if full:
        cmd += ["-full-screen"]
    if debug:
        cmd += ["-d", "int,cpu_reset", "-no-reboot", "-no-shutdown"]

    print("\n==> 启动 QEMU:")
    print("    " + " ".join(cmd))
    print("    串口输出直接显示在本终端; 引导时先出现 BIOS 信息属正常现象.\n")
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
