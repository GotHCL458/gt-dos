#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""TC 性能基准与对比:
   在 GT-DOS 内用 TC 编译并运行 \SDK\BENCH.C, 记录各工作负载耗时;
   同时在宿主用 rustc -O 编译 tools/bench_host.rs 运行同一组计算.
   对比 GT-DOS(TC 生成代码, QEMU) 与宿主原生的吞吐, 并比较
   直接调用 vs 函数指针调用 等语言特性开销."""

import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = r"D:\GT-DOS"
QEMU = os.path.join(ROOT, "qemu", "qemu-system-x86_64.exe")
IMG = os.path.join(ROOT, "build", "gt-dos-hd.img")
TMP = os.path.join(ROOT, "build", "perfrun")
REPORT = os.path.join(ROOT, "build", "perf_report.md")
SPORT = 4900 + (os.getpid() % 80)
MPORT = SPORT + 1
FAILURES = []

# 工作负载: 名称 -> (迭代次数, 说明)
WORK = [
    ("int_loop", 5_000_000, "整数累加"),
    ("direct_call", 2_000_000, "直接函数调用"),
    ("indirect_call", 2_000_000, "函数指针调用"),
    ("double_ops", 2_000_000, "double 乘加"),
    ("array_sum", 5_000_000, "数组索引读取"),
    ("fib30", 0, "递归 fib(30)"),
]


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" +
          (f"  ({detail})" if detail and not cond else ""), flush=True)
    if not cond:
        FAILURES.append(name)


def stage(msg):
    print(f"--- {msg}", flush=True)


class VM:
    def __init__(self):
        os.makedirs(TMP, exist_ok=True)
        shutil.copy(IMG, os.path.join(TMP, "hd.img"))
        args = [QEMU, "-m", "32", "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT)
        self.m = self._conn(MPORT)
        self.buf = ""

    @staticmethod
    def _conn(port, t=8.0):
        end = time.time() + t
        while time.time() < end:
            try:
                return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError:
                time.sleep(0.02)
        raise RuntimeError(port)

    def feed(self, ms=500):
        self.s.settimeout(0.2)
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

    def cmd(self, line, wait_ms=700):
        try:
            self.s.sendall((line + "\r").encode())
        except OSError:
            pass
        self.feed(wait_ms)

    def expect(self, needle, timeout_ms=6000):
        end = time.time() + timeout_ms / 1000
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(200)
        return False

    def close(self):
        for x in (self.s, self.m):
            try:
                x.close()
            except Exception:
                pass
        if self.p.poll() is None:
            self.p.kill()
            try:
                self.p.wait(5)
            except Exception:
                pass


def build_host():
    """编译宿主原生基线; 失败返回 None."""
    exe = os.path.join(ROOT, "build", "bench_host.exe")
    src = os.path.join(ROOT, "tools", "bench_host.rs")
    try:
        r = subprocess.run(["rustc", "-O", src, "-o", exe],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            print(r.stderr[-400:], flush=True)
            return None
        return exe
    except Exception as e:
        print(f"rustc unavailable: {e}", flush=True)
        return None


def run_host(exe):
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    except Exception as e:
        print(f"host run failed: {e}", flush=True)
        return {}
    out = {}
    for line in r.stdout.splitlines():
        m = re.match(r"HOST (\w+) us=(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))   # 微秒
    return out


def parse_guest(buf):
    out = {}
    for m in re.finditer(r"BENCH (\w+) ms=(\d+) chk=(\d+)", buf):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return out


def ns_per_op(ms, iters):
    if iters <= 0:
        return 0.0
    return ms * 1_000_000.0 / iters


def main():
    stage("host baseline")
    exe = build_host()
    host = run_host(exe) if exe else {}
    check("host baseline built", exe is not None)
    if host:
        print("  host us: " + ", ".join(f"{k}={v}" for k, v in host.items()),
              flush=True)

    stage("boot GT-DOS")
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

    stage("compile bench.c (TC)")
    vm.buf = ""
    t0 = time.time()
    vm.cmd("tc \\sdk\\bench.c", 0)
    ok = vm.expect("Compiled ", 20000)
    compile_ms = int((time.time() - t0) * 1000)
    m = re.search(r"Compiled (\d+) bytes", vm.buf)
    code_bytes = int(m.group(1)) if m else 0
    check("tc compiles bench", ok, vm.buf[-400:])
    check("bench image size", 1000 <= code_bytes <= 262144,
          f"{code_bytes} bytes")
    print(f"  compile={compile_ms} ms, image={code_bytes} bytes", flush=True)

    stage("run bench (GT-DOS)")
    vm.buf = ""
    vm.cmd("run bench.gtp", 0)
    check("bench runs to completion", vm.expect("BENCH fib30", 60000),
          vm.buf[-400:])
    guest = parse_guest(vm.buf)
    check("all workloads reported", len(guest) >= len(WORK),
          f"got {sorted(guest)}")
    for name, _, _ in WORK:
        g = guest.get(name)
        check(f"{name} chk", g is not None and g[1] == 1, str(g))

    # 语言特性对比: 直接调用 vs 函数指针调用
    d = guest.get("direct_call", (0, 0))[0]
    ind = guest.get("indirect_call", (0, 0))[0]
    ratio = (ind / d) if d > 0 else 0.0
    check("indirect call not pathological", 0 < d and 0 < ind and ratio < 20,
          f"direct={d}ms indirect={ind}ms")
    print(f"  direct={d}ms  indirect={ind}ms  ratio={ratio:.2f}x", flush=True)

    vm.close()

    # ---- 汇总表 ----
    rows = []
    for name, iters, desc in WORK:
        g = guest.get(name)
        gms = g[0] if g else 0
        h_us = host.get(name, 0)
        hms = h_us / 1000.0
        gns = ns_per_op(gms, iters)
        hns = (h_us * 1000.0 / iters) if iters > 0 else 0.0
        slow = (gms * 1000.0 / h_us) if h_us > 0 else 0.0
        rows.append((name, desc, iters, gms, hms, gns, hns, slow))

    print()
    print(f"{'workload':14s} {'iters':>9s} {'GT(ms)':>8s} "
          f"{'host(ms)':>9s} {'GT ns/op':>10s} {'host ns/op':>11s} "
          f"{'slowdown':>9s}")
    for r in rows:
        print(f"{r[0]:14s} {r[2]:9d} {r[3]:8d} {r[4]:9.3f} "
              f"{r[5]:10.2f} {r[6]:11.2f} "
              f"{(f'{r[7]:.1f}x' if r[7] else '-'):>9s}")

    lines = [
        "# TC 性能对比",
        "",
        f"- 环境: GT-DOS in QEMU (32MB, TCG) vs 宿主原生 (rustc -O)",
        f"- TC 编译: {compile_ms} ms, 程序镜像 {code_bytes} 字节",
        f"- 直接调用 {d} ms vs 函数指针调用 {ind} ms "
        f"(x{ratio:.2f})",
        "",
        "| 工作负载 | 迭代 | GT-DOS ms | 宿主 ms | GT ns/op | 宿主 ns/op | 慢倍数 |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        slow = f"{r[7]:.1f}x" if r[7] else "-"
        lines.append(f"| {r[0]} ({r[1]}) | {r[2]} | {r[3]} | {r[4]:.3f} | "
                     f"{r[5]:.2f} | {r[6]:.2f} | {slow} |")
    lines += [
        "",
        "说明: 宿主列是原生 x86-64 执行, GT-DOS 列运行在 QEMU TCG 上,",
        "倍数主要反映模拟器开销 + TC 未优化代码生成; ns/op 用于横向比较",
        "同一平台内不同语言特性 (如直接调用 vs 函数指针调用).",
        "",
    ]
    try:
        with open(REPORT, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        check("perf report written", os.path.exists(REPORT))
    except OSError as e:
        check("perf report written", False, str(e))

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
