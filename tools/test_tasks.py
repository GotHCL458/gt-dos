#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS 任务/系统服务回归: SYS 任务显示、服务程序运行、SYS 保护."""

import os
import shutil
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
IMG = r"D:\GT-DOS\build\gt-dos-hd.img"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4860 + (os.getpid() % 40)
FAILURES = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" +
          (f"  ({detail})" if detail and not cond else ""), flush=True)
    if not cond:
        FAILURES.append(name)


def connect(port, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 0.5)
        except OSError:
            time.sleep(0.03)
    raise RuntimeError(f"cannot connect to port {port}")


class VM:
    def __init__(self):
        os.makedirs(TMP, exist_ok=True)
        shutil.copy(IMG, os.path.join(TMP, "hd.img"))
        args = [QEMU, "-m", "32", "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = connect(SPORT)
        self.s.settimeout(0.15)
        self.buf = ""

    def feed(self, ms=400):
        end = time.time() + ms / 1000
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d:
                    self.buf += d.decode("utf-8", "replace")
            except socket.timeout:
                pass
            except OSError:
                break

    def cmd(self, line, ms=700):
        self.s.sendall((line + "\r").encode())
        self.feed(ms)

    def expect(self, needle, ms=6000):
        end = time.time() + ms / 1000
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(200)
        return False

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass
        if self.p.poll() is None:
            self.p.kill()
            try:
                self.p.wait(5)
            except Exception:
                pass


def main():
    vm = VM()
    vm.feed(3000)
    vm.expect("Language", 8000)
    for s in ("1", "SBOX", "admin", ""):
        vm.cmd(s, 600)
    vm.expect("Enjoy", 8000)
    vm.s.sendall(b"\x1b")
    vm.feed(600)
    vm.expect("User:", 8000)
    vm.cmd("admin", 600)
    vm.expect("Password", 3000)
    vm.cmd("", 800)
    check("login", vm.expect(">", 5000))

    # 1) 内核系统服务以 SYS 登记
    vm.buf = ""
    vm.cmd("tasks", 900)
    check("tasks lists kernel services", vm.expect("kernel.core", 4000),
          vm.buf[-300:])
    check("tasks lists protected image", vm.expect("kernel.image", 4000),
          vm.buf[-300:])
    check("tasks marks SYS", vm.expect("SYS", 4000), vm.buf[-300:])
    check("tasks marks protected", vm.expect("PROT", 4000) and
          vm.expect("yes", 4000), vm.buf[-300:])
    check("tasks shows system owner", vm.expect("SYSTEM", 4000), vm.buf[-300:])

    # 1b) 内核容器自检结果与磁盘受保护视图
    vm.buf = ""
    vm.cmd("kernel", 1200)
    check("kernel container GTK1", vm.expect("GTK1", 4000), vm.buf[-300:])
    check("kernel shows SHA-256", vm.expect("SHA-256", 4000), vm.buf[-300:])
    check("kernel runtime check verified", vm.expect("Runtime check", 4000)
          and vm.expect("verified", 4000), vm.buf[-300:])
    check("kernel disk image matches", vm.expect("matches", 4000),
          vm.buf[-300:])

    # 1b-2) 运行时只读段复核 (防篡改)
    vm.buf = ""
    vm.cmd("kernel verify", 700)
    check("kernel verify reports ok", vm.expect("runtime check", 4000)
          and vm.expect("verified", 4000) and
          not vm.expect("FAILED", 300), vm.buf[-300:])

    # 1c) 内核镜像的 .gtp 视图不可作为程序执行
    vm.buf = ""
    vm.cmd("run \\GT-DOS\\KERNEL.GTP", 1200)
    check("kernel image not runnable", vm.expect("protected", 4000),
          vm.buf[-300:])

    # 2) 服务清单: \GT-DOS\SERVICES 下加密的 .gtp
    vm.buf = ""
    vm.cmd("service list", 900)
    check("service list shows netsvc", vm.expect("netsvc.gtp", 4000),
          vm.buf[-300:])
    check("service list marks [SYS]", vm.expect("[SYS]", 4000), vm.buf[-300:])

    # 3) 运行系统服务 (以 SYS 任务执行, 输出服务标识)
    vm.buf = ""
    vm.cmd("service run netsvc", 2500)
    check("service runs", vm.expect("GT-DOS service: net agent", 8000),
          vm.buf[-300:])
    check("service exit code", vm.expect("exited with code 0", 6000),
          vm.buf[-300:])

    # 4) SYS 服务受保护, 不能停止
    vm.buf = ""
    vm.cmd("service stop netsvc", 900)
    check("sys service protected from stop",
          vm.expect("protected", 4000), vm.buf[-300:])

    # 5) 服务文件位于 \GT-DOS\ 下, 不可删除/修改
    vm.buf = ""
    vm.cmd("del \\GT-DOS\\SERVICES\\NETSVC.GTP", 900)
    check("service file delete denied",
          vm.expect("protected", 4000) or vm.expect("denied", 4000),
          vm.buf[-300:])

    vm.buf = ""
    vm.cmd("attrib \\GT-DOS\\SERVICES\\NETSVC.GTP -r", 900)
    check("service attrib denied",
          vm.expect("protected", 4000) or vm.expect("denied", 4000),
          vm.buf[-300:])

    # 6) 普通 .gtp 仍可作为 USR 程序运行
    vm.buf = ""
    vm.cmd("run hello.gtp", 1500)
    check("user program still runs", vm.expect("Hello from GT-DOS", 5000),
          vm.buf[-300:])

    vm.cmd("shutdown"); vm.feed(600); vm.close()
    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
