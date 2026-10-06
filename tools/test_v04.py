#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS v0.7 回归: 单盘 C: + GT-DOS 目录 + INCLUDE 头文件 + 属性 +
   busybox 命令 + DIR /A + 系统保护 + 中文显示(串口 UTF-8)."""
import os, shutil, socket, subprocess, sys, time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4600 + (os.getpid() % 80)
MPORT = SPORT + 1

FAILURES = []

def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)

class VM:
    def __init__(self):
        args = [QEMU, "-m", "32",
                "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT); self.m = self._conn(MPORT)
        self.buf = ""
    @staticmethod
    def _conn(port, t=8.0):
        end = time.time() + t
        while time.time() < end:
            try: return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError: time.sleep(0.02)
        raise RuntimeError(port)
    def feed(self, ms=500):
        self.s.settimeout(0.2)
        end = time.time() + ms/1000
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
        end = time.time() + timeout_ms/1000
        low = needle.lower()
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

    vm = VM()
    time.sleep(0.5)
    if vm.p.poll() is not None:
        print("QEMU failed to start (port busy?)"); return 1
    vm.feed(3000)
    # 逐个轮询 OOBE 提示 (连接稳定后每个提示都能抓到)
    if not vm.expect("Language", 6000):
        vm.send("\x1b"); vm.feed(300)
        vm.expect("Language", 3000)
    vm.cmd("1")
    vm.expect("Host name", 3000); vm.cmd("TBOX")
    vm.expect("user name", 3000); vm.cmd("admin")
    vm.expect("Password", 3000); vm.cmd("")
    vm.expect("Choose", 3000); vm.cmd("1")
    check("oobe done", vm.expect("Enjoy GT-DOS", 6000), vm.buf[-400:])
    vm.send("\x1b"); vm.feed(500)
    check("login screen", vm.expect("User:", 8000))
    vm.cmd("")
    check("empty user rejected", vm.expect("cannot be empty", 3000))
    vm.cmd("admin")
    vm.expect("Password", 3000)
    vm.cmd("")                         # 无口令, 空回车提交
    check("at prompt", vm.expect(">", 4000))

    # ---- 盘符: 只有 C: ----
    vm.cmd("drives")
    check("drives shows C:", vm.expect("C:", 3000))
    vm.cmd("df", 1000)
    check("df C:", vm.expect("Drive C:", 3000))
    vm.cmd("D:")
    check("D: invalid", vm.expect("Invalid drive", 3000))
    vm.cmd("S:")
    check("S: invalid", vm.expect("Invalid drive", 3000))
    vm.cmd("pwd")
    check("pwd root", vm.expect("C:\\", 3000))

    # ---- GT-DOS 目录与 INCLUDE 头文件 (真实文件) ----
    vm.cmd("dir")
    check("root dir shows GT-DOS", vm.expect("gt-dos", 4000))
    vm.cmd("dir /a gt-dos\\include")
    check("INCLUDE shows stdio.h", vm.expect("stdio.h", 4000))
    vm.cmd("cat gt-dos\\include\\stdio.h")
    check("stdio.h content", vm.expect("_GT_STDIO_H", 3000))
    vm.cmd("put gt-dos\\hack.txt x")
    check("write into GT-DOS blocked", vm.expect("protected", 3000))

    # ---- busybox 命令 ----
    vm.cmd("touch scratch.txt")
    check("touch creates", vm.expect("Touched", 3000))
    vm.cmd("put scratch.txt alpha beta gamma")
    vm.cmd("wc scratch.txt")
    check("wc counts", vm.expect("0 3 16", 3000))
    vm.cmd("cp scratch.txt copy.txt")
    check("cp works", vm.expect("Copied", 3000))
    vm.cmd("mv copy.txt moved.txt")
    check("mv works", vm.expect("Moved", 3000))
    vm.cmd("dir")
    check("moved.txt exists", vm.expect("moved.txt", 3000))

    # ---- 属性 ----
    vm.cmd("attrib moved.txt +r")
    check("attrib +r", vm.expect("R--A", 3000))
    vm.cmd("del moved.txt")
    check("read-only delete blocked", vm.expect("Cannot delete", 3000))
    vm.cmd("attrib moved.txt -r")
    check("attrib -r", vm.expect("---A", 3000))
    vm.cmd("del moved.txt")
    check("delete after -r", vm.expect("Deleted", 3000))
    vm.cmd("del scratch.txt")

    # ---- mkdir/rmdir/tree ----
    vm.cmd("mkdir docs")
    check("mkdir", vm.expect("Created directory", 3000))
    vm.cmd("tree")
    check("tree shows docs", vm.expect("docs", 3000))
    vm.cmd("rmdir docs")
    check("rmdir empty", vm.expect("Removed", 3000))

    # ---- 系统保护: admin 内置账户不可删 ----
    vm.cmd("userdel admin")
    check("cannot delete built-in admin",
          vm.expect("Cannot delete", 3000) or vm.expect("protected", 3000))

    # ---- 中断嵌套 / APIC ----
    vm.cmd("irqstat")
    check("irqstat shows PIC mode", vm.expect("8259A PIC", 3000))
    vm.cmd("apic on")
    check("apic enabled", vm.expect("APIC mode enabled", 3000))
    vm.cmd("irqstat")
    check("irqstat shows APIC", vm.expect("IRQ controller : APIC", 3000))
    vm.cmd("put apic_test.txt works-under-apic")
    check("keyboard works under APIC", vm.expect("Wrote", 3000))
    vm.cmd("cat apic_test.txt")
    check("file r/w under APIC", vm.expect("works-under-apic", 3000))
    vm.cmd("apic off")
    check("back to PIC", vm.expect("APIC disabled", 3000))
    vm.cmd("put pic_test.txt back-to-pic")
    check("keyboard works after APIC off", vm.expect("Wrote", 3000))
    vm.cmd("del apic_test.txt"); vm.cmd("del pic_test.txt")

    # ---- 持久化: 建文件, 重启后仍在 ----
    vm.cmd("del persist.txt")
    vm.feed(300)
    vm.cmd("put persist.txt survived-reboot")
    check("persist written", vm.expect("to C:\\persist.txt", 3000), vm.buf[-200:])
    vm.buf = ""
    vm.cmd("dir")
    check("persist in dir", vm.expect("persist.txt", 3000), vm.buf[-200:])
    vm.feed(1500)
    vm.close()
    time.sleep(0.4)
    vm = VM()
    vm.feed(2500)
    vm.send("\x1b"); vm.feed(500)
    check("login after reboot", vm.expect("User:", 8000))
    vm.cmd("admin")                    # 用户名不能为空
    vm.expect("Password", 3000)
    vm.cmd("")
    check("at prompt after reboot", vm.expect(">", 4000))
    vm.cmd("cat persist.txt")
    check("persist across reboot", vm.expect("survived-reboot", 5000), vm.buf[-300:])

    vm.cmd("shutdown")
    vm.feed(800)
    vm.close()

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
