#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
GT-DOS 构建脚本
=======================================================================
  python build.py            # 编译, 生成 build/gt-dos-hd.img
  python build.py run        # 只运行 QEMU (不重新编译)
  python build.py run -nogui # 运行, 只输出到终端(串口), 不开图形窗口
  python build.py run -debug # 运行, 打开 CPU/中断跟踪日志
  python build.py all        # 先编译再运行
  python build.py clean      # 清理 build 目录

磁盘布局 (单 HDD 镜像, MBR 分区):
  LBA0        : MBR (boot/mbr.asm, 分区表由本脚本写入)
  part1 C:    : FAT16 系统盘 (含 \GT-DOS\ 系统目录与 INCLUDE 头文件)
  part2 隐藏  : 盘尾启动分区 (type 0x11, bootable):
                [boot.bin | kernel.bin | font.bin | cpm.bin]
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


# --------------------------------------------------------------------- FAT16 卷
FAT_SECS_PER_CLUS = 4
FAT_RESERVED = 1
FAT_NUM_FATS = 2
FAT_ROOT_ENTRIES = 512

def _le16(v): return bytes([v & 0xFF, (v >> 8) & 0xFF])
def _le32(v): return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def make_fat16_volume(total, seed_files):
    """生成 FAT16 卷 (分区内布局, 不含分区表), 返回 bytes.

    seed_files: {名字: 内容}, 内容可为:
      bytes            普通文件 (ATTR_ARC)
      dict             目录
      (attr, bytes)    带属性文件
      (attr, dict)     带属性目录
    """
    root_sectors = (FAT_ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    fat_secs = 1
    for _ in range(8):
        clusters = (total - FAT_RESERVED - FAT_NUM_FATS * fat_secs - root_sectors) // FAT_SECS_PER_CLUS
        new_fat = max(1, (clusters * 2 + SECTOR + 1 + SECTOR - 1) // SECTOR)
        if new_fat == fat_secs:
            break
        fat_secs = new_fat
    clusters = (total - FAT_RESERVED - FAT_NUM_FATS * fat_secs - root_sectors) // FAT_SECS_PER_CLUS
    if clusters < 4085 or clusters > 65524:
        raise SystemExit(f"[!] FAT16 簇数异常: {clusters}")

    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"GTDOSFAT"
    bs[11:13] = _le16(512)
    bs[13] = FAT_SECS_PER_CLUS
    bs[14:16] = _le16(FAT_RESERVED)
    bs[16] = FAT_NUM_FATS
    bs[17:19] = _le16(FAT_ROOT_ENTRIES)
    bs[19:21] = _le16(total if total < 65536 else 0)
    bs[21] = 0xF8
    bs[22:24] = _le16(fat_secs)
    bs[24:26] = _le16(63)
    bs[26:28] = _le16(255)
    bs[28:32] = _le32(0)             # hidden sectors (fat.c 用分区表偏移)
    bs[32:36] = _le32(total if total >= 65536 else 0)
    bs[36] = 0x80
    bs[38] = 0x29
    bs[39:43] = _le32(0x12345678)
    bs[43:54] = b"GT-DOS C   "[:11]
    bs[54:62] = b"FAT16   "
    bs[510] = 0x55
    bs[511] = 0xAA

    fat = bytearray(fat_secs * SECTOR)
    fat[0:2] = _le16(0xFFF8)
    fat[2:4] = _le16(0xFFFF)
    fat_image = bytearray(fat) * FAT_NUM_FATS

    root = bytearray(root_sectors * SECTOR)

    def dir_slot(name83, attr, first_clus, size):
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        e[26:28] = _le16(first_clus)
        e[28:32] = _le32(size)
        return e

    data_region = bytearray((total - FAT_RESERVED - FAT_NUM_FATS * fat_secs - root_sectors) * SECTOR)
    next_cluster = [2]
    root_entries = []

    def alloc_chain(nbytes):
        nclus = max(1, (nbytes + FAT_SECS_PER_CLUS * SECTOR - 1)
                    // (FAT_SECS_PER_CLUS * SECTOR))
        first = next_cluster[0]
        for i in range(nclus):
            c = first + i
            nxt = c + 1 if i + 1 < nclus else 0xFFFF
            off = c * 2
            fat_image[off:off + 2] = _le16(nxt)
            fat_image[(fat_secs * SECTOR) + off:(fat_secs * SECTOR) + off + 2] = _le16(nxt)
        next_cluster[0] += nclus
        return first, nclus

    def write_data(first, content):
        coff = (first - 2) * FAT_SECS_PER_CLUS * SECTOR
        data_region[coff:coff + len(content)] = content

    def name83(fname):
        raw = bytearray(b" " * 11)
        up = fname.upper().encode("ascii")
        dot = up.find(b".")
        if dot < 0:
            raw[0:len(up)] = up
        else:
            raw[0:dot] = up[:dot]
            raw[8:8 + len(up) - dot - 1] = up[dot + 1:]
        return bytes(raw)

    def split_attr(val):
        """返回 (attr, payload)."""
        if isinstance(val, tuple):
            return val
        if isinstance(val, dict):
            return 0x10, val
        return 0x20, val

    def build_dir_entries(items, first_clus, self_name, parent_clus):
        dsect = bytearray(FAT_SECS_PER_CLUS * SECTOR)
        idx = 0
        e = bytearray(32); e[0:11] = b"." + b" " * 10
        e[11] = 0x10; e[26:28] = _le16(first_clus & 0xFFFF)
        e[28:32] = _le32(0); dsect[idx:idx+32] = e; idx += 32
        e = bytearray(32); e[0:11] = b".." + b" " * 9
        e[11] = 0x10; e[26:28] = _le16(parent_clus & 0xFFFF)
        e[28:32] = _le32(0); dsect[idx:idx+32] = e; idx += 32
        for name, val in items.items():
            attr, payload = split_attr(val)
            if isinstance(payload, dict):
                sub_first, _ = alloc_chain(0)
                build_dir_entries(payload, sub_first, name, first_clus)
                e = dir_slot(name83(name), attr | 0x10, sub_first, 0)
            else:
                f_first, _ = alloc_chain(len(payload))
                write_data(f_first, payload)
                e = dir_slot(name83(name), attr & ~0x10, f_first, len(payload))
            dsect[idx:idx+32] = e; idx += 32
        coff = (first_clus - 2) * FAT_SECS_PER_CLUS * SECTOR
        data_region[coff:coff + len(dsect)] = dsect

    root_entries.append(dir_slot(b"GT-DOS C  ", 0x08, 0, 0))

    for fname, val in seed_files.items():
        attr, payload = split_attr(val)
        if isinstance(payload, dict):
            d_first, _ = alloc_chain(0)
            build_dir_entries(payload, d_first, fname, 0)
            root_entries.append(dir_slot(name83(fname), attr | 0x10, d_first, 0))
        else:
            f_first, _ = alloc_chain(len(payload))
            write_data(f_first, payload)
            root_entries.append(dir_slot(name83(fname), attr & ~0x10, f_first, len(payload)))

    for i, e in enumerate(root_entries):
        root[i * 32:(i + 1) * 32] = e

    img = bytearray()
    img += bs
    img += fat_image
    img += root
    img += data_region
    assert len(img) == total * SECTOR, f"{len(img)} != {total * SECTOR}"
    return bytes(img), clusters


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
    "-fno-stack-protector",
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
    "gui.c",
    "keyboard.c",
    "mouse.c",
    "timer.c",
    "lib.c",
    "ata.c",
    "fat.c",
    "cfg.c",
    "user.c",
    "power.c",
    "oobe.c",
    "shell.c",
]

ASM_SOURCES = ["entry.asm", "isr.asm"]


def ensure_font():
    """font.bin / cpm.bin 缺失时自动调用 tools/mkfont.py 生成."""
    fpath = os.path.join(BUILD, "font.bin")
    cpath = os.path.join(BUILD, "cpm.bin")
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
    out = os.path.join(BUILD, "boot.bin")
    run([TOOLS["nasm"], "-f", "bin", src, "-o", out])
    data = open(out, "rb").read()
    if len(data) != SECTOR * 16:
        print(f"[!] stage2 大小异常: {len(data)} (应为 {SECTOR * 16})")
        sys.exit(1)
    return data


def build_mbr():
    src = os.path.join(ROOT, "boot", "mbr.asm")
    out = os.path.join(BUILD, "mbr.bin")
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

    bin_ = os.path.join(BUILD, "kernel.bin")
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


def load_include_seed():
    """kernel/include/*.h -> {名字: bytes}, 写入 C:\\GT-DOS\\INCLUDE\\."""
    inc_dir = os.path.join(ROOT, "kernel", "include")
    files = {}
    if os.path.isdir(inc_dir):
        for fn in sorted(os.listdir(inc_dir)):
            if fn.endswith(".h"):
                with open(os.path.join(inc_dir, fn), "rb") as f:
                    files[fn.upper()] = f.read()
    return files


def make_hd_image(mbr, boot2, kernel, font, cpm):
    """组装单 HDD 镜像: MBR + C: FAT16 + 盘尾启动分区."""
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

    # C: 分区 FAT16 卷
    attr_hdr = 0x01 | 0x04 | 0x20                # R|S|A (系统头文件, 受保护)
    include = load_include_seed()
    seed = {
        "GT-DOS": {                              # 系统目录 (可见, 名称保护)
            "INCLUDE": {
                n: (attr_hdr, d) for n, d in include.items()
            },
        },
    }
    fat_img, clusters = make_fat16_volume(PART1_SECTORS, seed)

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
    mbr[0x1BE:0x1CE] = pe(0x00, 0x06, PART1_LBA, PART1_SECTORS)
    mbr[0x1CE:0x1DE] = pe(0x80, 0x11, BOOT_PART_LBA, BOOT_PART_SECTORS)

    img = bytearray(DISK_SECTORS * SECTOR)
    img[0:SECTOR] = mbr
    img[PART1_LBA * SECTOR:PART1_LBA * SECTOR + len(fat_img)] = fat_img
    img[BOOT_PART_LBA * SECTOR:] = bootpart

    path = os.path.join(BUILD, "gt-dos-hd.img")
    with open(path, "wb") as f:
        f.write(img)
    print(f"    硬盘镜像 : {path} ({DISK_SECTORS * SECTOR} 字节)")
    print(f"    C: 分区  : LBA {PART1_LBA} + {PART1_SECTORS} 扇区 "
          f"({clusters} 簇 FAT16)")
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
    font = open(os.path.join(BUILD, "font.bin"), "rb").read()
    cpm = open(os.path.join(BUILD, "cpm.bin"), "rb").read()
    print("==> 组装单硬盘镜像 (MBR + C: FAT16 + 隐藏启动分区)")
    hd = make_hd_image(mbr, boot2, kernel, font, cpm)
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


def run_qemu(hd, nogui=False, debug=False):
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
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    if nogui:
        cmd += ["-display", "none"]
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
                 debug=("-debug" in args or "--debug" in args))


if __name__ == "__main__":
    main()
