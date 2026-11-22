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
import shutil
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

MAX_KERNEL_SECTORS = 640            # 内核上限 320KB (0x10000..0x60000)

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
        extra = ["-Os"] if name in ("tc.c", "asm.c", "sched.c", "gui.c", "richtext.c") else []
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
