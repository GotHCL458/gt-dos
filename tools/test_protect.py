#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS 内核容器 / .gtp 完整性回归.

  1) 篡改内核镜像中的只读字符串 -> 启动自检失败, panic 不进入系统
  2) 篡改磁盘上的 hello.gtp 数据 -> RUN 拒绝执行 (CRC 不符)"""

import os
import shutil
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
IMG = r"D:\GT-DOS\build\gt-dos-hd.img"
TMP = r"D:\GT-DOS\build\testprotect"
SPORT = 4920 + (os.getpid() % 40)
FAILURES = []

KERNEL_MSG = b"kernel container GTK1 verified"
HELLO_MSG = b"Hello from GT-DOS INT 0x21!"


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
    def __init__(self, img):
        os.makedirs(TMP, exist_ok=True)
        self.path = os.path.join(TMP, f"hd-{int(time.time()*1000)%100000}.img")
        shutil.copy(img, self.path)
        args = [QEMU, "-m", "32", "-hda", self.path,
                "-boot", "c", "-display", "none",
                # 不带 nowait: QEMU 等待串口客户端就绪再启动, 避免早期 panic 输出丢失
                "-serial", f"tcp:127.0.0.1:{SPORT},server",
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


def login(vm):
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
    return vm.expect(">", 5000)


def tamper(src, dst, needle):
    data = bytearray(open(src, "rb").read())
    pos = data.find(needle)
    if pos < 0:
        raise RuntimeError(f"signature not found: {needle!r}")
    data[pos + len(needle) // 2] ^= 0xFF
    with open(dst, "wb") as f:
        f.write(data)
    return pos


def main():
    if not os.path.isfile(IMG):
        print("先运行 python build.py")
        return 1
    os.makedirs(TMP, exist_ok=True)

    # 1) 内核镜像被篡改: 启动自检必须拒绝
    bad_kernel = os.path.join(TMP, "kernel-bad.img")
    pos = tamper(IMG, bad_kernel, KERNEL_MSG)
    vm = VM(bad_kernel)
    vm.feed(6000)
    check("kernel tamper detected",
          vm.expect("INTEGRITY CHECK FAILED", 10000),
          f"sig@{pos} tail={vm.buf[-200:]}")
    check("kernel tamper halts before OOBE",
          not vm.expect("Language", 1500), vm.buf[-200:])
    vm.close()

    # 2) 磁盘上的 .gtp 被篡改: 程序完整性校验拒绝执行
    bad_gtp = os.path.join(TMP, "gtp-bad.img")
    pos = tamper(IMG, bad_gtp, HELLO_MSG)
    vm = VM(bad_gtp)
    check("tampered image still boots", login(vm), vm.buf[-200:])
    vm.buf = ""
    vm.cmd("run hello.gtp", 1500)
    check("gtp tamper rejected",
          vm.expect("integrity check failed", 5000), vm.buf[-300:])
    check("tampered program did not run",
          not vm.expect("Hello from GT-DOS", 2000), vm.buf[-300:])
    vm.cmd("shutdown")
    vm.feed(600)
    vm.close()

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
