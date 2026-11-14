#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS v0.9 系统调用/程序执行验证:
   1) INT 0x21 门存在 (RUN HELLO.GTP 打印并由 SYS_EXIT 回跳)
   2) 退出码回传 (code 0)
   3) 缺失文件错误处理 (RUN NOPE.GTP)
   4) RUN 之后键盘/中断仍正常 (IF 经 exec_longjmp 恢复)"""
import os, shutil, socket, subprocess, sys, time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4700 + (os.getpid() % 80)
MPORT = SPORT + 1
FAILURES = []

def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)

class VM:
    def __init__(self):
        args = [QEMU, "-m", "32", "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT); self.m = self._conn(MPORT); self.buf = ""
    @staticmethod
    def _conn(port, t=8.0):
        end = time.time() + t
        while time.time() < end:
            try: return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError: time.sleep(0.02)
        raise RuntimeError(port)
    def feed(self, ms=500):
        self.s.settimeout(0.2); end = time.time() + ms/1000
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d: self.buf += d.decode("utf-8", "replace")
            except socket.timeout: pass
            except OSError: break
    def cmd(self, line, wait_ms=700):
        try: self.s.sendall((line + "\r").encode())
        except OSError: pass
        self.feed(wait_ms)
    def send(self, raw):
        try: self.s.sendall(raw.encode())
        except OSError: pass
    def expect(self, needle, timeout_ms=6000):
        end = time.time() + timeout_ms/1000; low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower(): return True
            self.feed(300)
        return False
    def close(self):
        for x in (self.s, self.m):
            try: x.close()
            except Exception: pass
        if self.p.poll() is None:
            self.p.kill()
            try: self.p.wait(5)
            except Exception: pass

def main():
    os.makedirs(TMP, exist_ok=True)
    shutil.copy(os.path.join(SRC, "gt-dos-hd.img"), os.path.join(TMP, "hd.img"))
    vm = VM(); vm.feed(3000)
    vm.expect("Language", 6000)
    vm.cmd("1"); vm.cmd("SBOX"); vm.cmd("admin"); vm.cmd("")
    check("oobe done", vm.expect("Enjoy GT-DOS", 6000), vm.buf[-300:])
    vm.send("\x1b"); vm.feed(500)
    check("login screen", vm.expect("User:", 8000))
    vm.cmd("admin"); vm.expect("Password", 3000); vm.cmd("")
    check("at prompt", vm.expect(">", 4000))

    vm.cmd("run hello.gtp", 1500)
    check("program output via INT 0x21",
          vm.expect("Hello from GT-DOS INT 0x21", 4000), vm.buf[-200:])
    check("exit code returned", vm.expect("exited with code 0", 3000), vm.buf[-200:])

    vm.buf = ""
    vm.cmd("run demo.gtp", 2500)
    check("gtd data read via open/read",
          vm.expect("GTD-DATA", 4000), vm.buf[-300:])
    check("seek + read", vm.expect("[DATA]", 3000), vm.buf[-300:])
    check("gtl shared library call", vm.expect("LIB OK", 3000), vm.buf[-300:])
    check("exfat xattr via statx", vm.expect("XATTR OK", 3000), vm.buf[-300:])
    check("demo exit code", vm.expect("exited with code 0", 3000), vm.buf[-300:])

    vm.buf = ""
    vm.cmd("run nope.gtp", 1000)
    check("missing file reported", vm.expect("RUN failed", 3000), vm.buf[-200:])

    vm.cmd("ver", 800)
    check("keyboard/interrupts alive after RUN", vm.expect("GT-DOS", 3000), vm.buf[-200:])
    vm.cmd("dir", 800)
    check("HELLO.GTP listed in dir", vm.expect("hello.gtp", 3000), vm.buf[-200:])

    vm.cmd("shutdown"); vm.feed(800); vm.close()
    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}"); return 1
    print("RESULT: ALL PASS"); return 0

if __name__ == "__main__":
    sys.exit(main())
