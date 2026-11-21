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

    # 按需 #include: 头文件 + 标准库模块 (typedef/enum/sizeof/printf)
    lib = [
        "#include <tc.h>",
        "#include <string.h>",
        "#include <stdlib.h>",
        "#include <stdio.h>",
        "#include <ctype.h>",
        "struct P { int x; int y; };",
        "typedef struct P Point;",
        "enum { BASE = 10, NEXT };",
        "int main(void) {",
        "char buf[32];",
        'strcpy(buf, "Hello");',
        'strcat(buf, " TC");',
        'printf("%s len=%d\\n", buf, strlen(buf));',
        'printf("atoi=%d abs=%d up=%c size=%d\\n",'
        ' atoi("-42"), abs(-7), toupper(\'a\'), sizeof(Point));',
        "Point p;",
        "p.x = BASE;",
        "p.y = NEXT;",
        'printf("p=%d,%d isz=%d\\n", p.x, p.y, sizeof(int));',
        "char *m = malloc(16);",
        "memset(m, 'A', 15);",
        "m[15] = 0;",
        'printf("heap=%s\\n", m);',
        "free(m);",
        "int r = rand();",
        'printf("rand_ok=%d\\n", r >= 0 ? 1 : 0);',
        'printf("sub=%s\\n", strstr(buf, "TC"));',
        'int hexv = strtol("0x1f", 0, 0);',
        'printf("hex=%d\\n", hexv);',
        'char *d = strdup("dup");',
        'printf("dup=%s\\n", d);',
        "free(d);",
        "int dn = 0;",
        "do { dn++; } while (dn < 3);",
        'printf("do=%d\\n", dn);',
        "int gi = 0;",
        "again:",
        "gi++;",
        "if (gi < 3) goto again;",
        'printf("goto=%d\\n", gi);',
        "return 0;",
        "}",
    ]
    vm.buf = ""
    vm.cmd(f"put lib.c {lib[0]}", 600)
    for line in lib[1:]:
        vm.cmd(f"append lib.c {line}", 450)
    vm.cmd("tc lib.c", 3000)
    check("tc compiles stdlib includes", vm.expect("Compiled ", 8000),
          vm.buf[-500:])
    vm.cmd("run lib.gtp", 2500)
    check("tc printf/string", vm.expect("Hello TC len=8", 5000),
          vm.buf[-500:])
    check("tc ctype/stdlib/sizeof",
          vm.expect("atoi=-42 abs=7 up=A size=16", 5000), vm.buf[-500:])
    check("tc typedef/enum", vm.expect("p=10,11 isz=8", 5000), vm.buf[-500:])
    check("tc malloc/memset", vm.expect("heap=AAAAAAAAAAAAAAA", 5000),
          vm.buf[-500:])
    check("tc rand", vm.expect("rand_ok=1", 5000), vm.buf[-500:])
    check("tc strstr/strtol", vm.expect("sub=TC", 4000) and
          vm.expect("hex=31", 4000), vm.buf[-500:])
    check("tc strdup/do-while", vm.expect("dup=dup", 4000) and
          vm.expect("do=3", 4000), vm.buf[-500:])
    check("tc goto/label", vm.expect("goto=3", 4000), vm.buf[-500:])

    # 未 include 时不编入库: strlen 应为未定义
    vm.buf = ""
    vm.cmd("put n.c int main(void){")
    vm.cmd('append n.c char *s = "x";')
    vm.cmd("append n.c return strlen(s);")
    vm.cmd("append n.c }")
    vm.cmd("tc n.c", 1500)
    check("tc library is on-demand", vm.expect("undefined function", 6000),
          vm.buf[-300:])

    # 函数指针 / 位域 / #define 宏 / 用户可变参数
    feat = [
        "#include <stdio.h>",
        "#include <stdarg.h>",
        "#define N 5",
        "#define MUL(a,b) ((a)*(b))",
        "#define SQUARE(x) MUL((x),(x))",
        "typedef int (*binop)(int,int);",
        "struct F { int (*cb)(int,int); int a:4; int b:6; unsigned c:3; };",
        "int add(int a,int b){ return a+b; }",
        "int sub(int a,int b){ return a-b; }",
        "int apply(binop f,int a,int b){ return f(a,b); }",
        "double gd = 1.5;",
        "double half(double x){ return x / 2.0; }",
        "int sum(int n, ...) {",
        "va_list ap;",
        "int s = 0;",
        "va_start(ap, n);",
        "for (int i = 0; i < n; i++) s += va_arg(ap, int);",
        "va_end(ap);",
        "return s;",
        "}",
        "int main(void) {",
        "int (*p)(int,int) = add;",
        'printf("fp1=%d\\n", p(7,3));',
        "p = sub;",
        'printf("fp2=%d\\n", p(7,3));',
        'printf("fp3=%d\\n", apply(add,4,5));',
        "struct F f;",
        "f.cb = add;",
        'printf("fp4=%d\\n", f.cb(2,9));',
        "struct F b;",
        "b.a = 5; b.b = 20; b.c = 7;",
        "b.a = b.a + 2;",
        "b.b += 3;",
        'printf("bit=%d,%d,%d sz=%d\\n", b.a, b.b, b.c, sizeof(struct F));',
        'printf("mac=%d %d %d\\n", N, MUL(3,4), SQUARE(5));',
        'printf("var=%d\\n", sum(4,1,2,3,4));',
        "double d = 2.5;",
        "double e = d * 3.0 + 1.25;",
        'printf("dbl=%f %f %d\\n", e, half(5.0), (int)(d + 0.6));',
        'printf("dcmp=%d %d gd=%f\\n", e > d, d == 2.5, gd + 1.0);',
        "return 0;",
        "}",
    ]
    vm.buf = ""
    vm.cmd(f"put feat.c {feat[0]}", 600)
    for line in feat[1:]:
        vm.cmd(f"append feat.c {line}", 450)
    vm.cmd("tc feat.c", 3000)
    check("tc functions-pointers/macros compile", vm.expect("Compiled ", 8000),
          vm.buf[-500:])
    vm.cmd("run feat.gtp", 2500)
    check("tc function pointers", vm.expect("fp1=10", 4000) and
          vm.expect("fp2=4", 4000) and vm.expect("fp3=9", 4000) and
          vm.expect("fp4=11", 4000), vm.buf[-500:])
    check("tc bitfields", vm.expect("bit=7,23,7 sz=16", 4000), vm.buf[-500:])
    check("tc #define macros", vm.expect("mac=5 12 25", 4000), vm.buf[-500:])
    check("tc variadic function", vm.expect("var=10", 4000), vm.buf[-500:])
    check("tc floating point", vm.expect("dbl=8.750000 2.500000 3", 4000),
          vm.buf[-500:])
    check("tc float compare/global", vm.expect("dcmp=1 1 gd=2.500000", 4000),
          vm.buf[-500:])

    # cat / rich / vim
    vm.buf = ""
    vm.cmd("cat \\sdk\\demo.md", 1000)
    check("cat reads file", vm.expect("GT-DOS Rich Text", 4000),
          vm.buf[-300:])
    vm.buf = ""
    vm.cmd("cat -n \\sdk\\demo.md", 1200)
    check("cat -n numbers lines", vm.expect("     1  ", 4000), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("rich \\sdk\\demo.md", 1500)
    check("rich renders markdown", vm.expect("GT-DOS Rich Text", 4000),
          vm.buf[-400:])
    check("rich table", vm.expect("Feature", 3000), vm.buf[-400:])

    # vim: 新文件 -> i 输入 -> ESC -> :wq -> cat 验证
    vm.buf = ""
    vm.cmd("vim vt.txt", 1200)
    vm.feed(800)
    vm.s.sendall(b"i")
    vm.feed(400)
    vm.s.sendall(b"hello vim")
    vm.feed(800)
    vm.s.sendall(b"\x1b")
    vm.feed(600)
    vm.cmd(":wq", 1500)
    check("vim returns to shell", vm.expect("VIM:", 5000), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("cat vt.txt", 1000)
    check("vim saved edit", vm.expect("hello vim", 4000), vm.buf[-300:])

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
