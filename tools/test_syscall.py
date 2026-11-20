#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS v0.10 系统调用/程序执行验证:
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
    vm.cmd("tc sdk\\hello.c", 1500)
    check("tc compiles hello", vm.expect("Compiled ", 6000), vm.buf[-300:])
    vm.cmd("run hello.gtp", 1500)
    check("tc hello output", vm.expect("Hello from TC!", 4000), vm.buf[-300:])
    check("tc print_int", vm.expect("42", 3000), vm.buf[-300:])
    check("tc hello exit", vm.expect("exited with code 0", 3000), vm.buf[-300:])

    vm.buf = ""
    vm.cmd("tc sdk\\fib.c", 1500)
    check("tc compiles fib", vm.expect("Compiled ", 6000), vm.buf[-300:])
    vm.cmd("run fib.gtp", 2000)
    check("tc fib recursion + loop",
          vm.expect("0 1 1 2 3 5 8 13 21 34", 4000), vm.buf[-300:])

    # 扩展语法: 数组 / 结构体 / 联合 / switch / 三目 / ++ --
    prog = [
        "struct P { int x; int y; };",
        "union U { int a; int b; };",
        "int g[4] = {3,1,4,1};",
        "struct P pts[2];",
        "char msg[8] = \"OK\";",
        "int add(int a, int b) { return a + b; }",
        "int sum(int *p, int n) { int s = 0; int i;"
        " for (i = 0; i < n; i++) s += p[i]; return s; }",
        "int main(void) {",
        "int i = 0;",
        "struct P p;",
        "struct P *q = &p;",
        "union U u;",
        "int loc[3] = {5,6,7};",
        "p.x = 10;",
        "q->y = 20;",
        "pts[0].x = 1;",
        "pts[1].y = 2;",
        "u.a = 5;",
        "print_int(u.b); print(\" \");",
        "int s = 0;",
        "for (i = 0; i < 4; i++) s += g[i];",
        "print(\"A\"); print_int(s); print(\" \");",
        "print(\"B\"); print_int(p.x + p.y); print(\" \");",
        "print(\"C\"); print_int(pts[0].x + pts[1].y); print(\" \");",
        "print(\"D\"); print_int(loc[1]); print(\" \");",
        "int t = i++ + ++i;",
        "print(\"E\"); print_int(t); print(\" \");",
        "switch (s) {",
        "case 8: print(\"CASE8\"); break;",
        "case 9: print(\"CASE9\"); break;",
        "default: print(\"OTHER\"); break;",
        "}",
        "print(\" \");",
        "print(s == 9 ? \"T\" : \"F\");",
        "print(\" \");",
        "print(msg);",
        "print(\" \");",
        "print_int(add(sum(g, 4), 1));",
        "putc('\\n');",
        "return 0;",
        "}",
    ]
    vm.buf = ""
    vm.cmd(f"put f.c {prog[0]}", 600)
    for line in prog[1:]:
        vm.cmd(f"append f.c {line}", 450)
    vm.cmd("tc f.c", 2000)
    check("tc compiles extended syntax", vm.expect("Compiled ", 8000),
          vm.buf[-400:])
    vm.cmd("run f.gtp", 2000)
    check("tc arrays/structs/switch/ternary",
          vm.expect("5 A9 B30 C3 D6 E10 CASE9 T OK 10", 6000),
          vm.buf[-400:])
    check("tc extended exit code", vm.expect("exited with code 0", 3000),
          vm.buf[-300:])

    vm.buf = ""
    vm.cmd("put t.c int main(void){")
    vm.cmd('append t.c print("PUT OK\\n");')
    vm.cmd("append t.c return 0;")
    vm.cmd("append t.c }")
    vm.cmd("tc t.c", 1500)
    check("tc compiles put/append source", vm.expect("Compiled ", 6000), vm.buf[-300:])
    vm.cmd("run t.gtp", 1500)
    check("tc put/append program runs", vm.expect("PUT OK", 4000), vm.buf[-300:])

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
