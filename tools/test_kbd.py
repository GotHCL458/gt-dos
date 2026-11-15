#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS 键盘回归: 只用 QEMU monitor 的 sendkey 注入 PS/2 按键, 串口观察结果.
   覆盖 NumLock/小键盘、CapsLock、Ctrl+C、Ctrl+Up 滚动、Ctrl+Alt+Del 重启."""

import os
import shutil
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
IMG = r"D:\GT-DOS\build\gt-dos-hd.img"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4810 + (os.getpid() % 60)
MPORT = SPORT + 100
FAILURES = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" +
          (f"  ({detail})" if detail and not cond else ""), flush=True)
    if not cond:
        FAILURES.append(name)


def stage(msg):
    print(f"--- {msg}", flush=True)


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
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = connect(SPORT)
        self.m = connect(MPORT)
        self.s.settimeout(0.2)
        self.m.settimeout(0.3)
        self.buf = ""

    def feed(self, ms=350):
        self.s.settimeout(0.15)
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

    def serial_cmd(self, line, ms=700):
        self.s.sendall((line + "\r").encode())
        self.feed(ms)

    def key(self, name, ms=120):
        self.m.sendall(f"sendkey {name}\n".encode())
        time.sleep(ms / 1000)
        try:
            self.m.recv(4096)
        except Exception:
            pass
        self.feed(120)

    def keys(self, names, ms=120):
        for n in names:
            self.key(n, ms)

    def expect(self, needle, ms=6000):
        end = time.time() + ms / 1000
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(200)
        return False

    def dump(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.m.sendall(f"screendump {path}\n".encode())
        time.sleep(0.35)
        try:
            self.m.recv(4096)
        except Exception:
            pass

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


def yellow_pixels(path, wh):
    """统计右下区域黄色像素 (滚动提示条)."""
    w, h = wh
    with open(path, "rb") as f:
        data = f.read()
    i, toks = 0, []
    while len(toks) < 4:
        while i < len(data) and data[i] in (10, 32, 9):
            i += 1
        s = i
        while i < len(data) and data[i] not in (10, 32, 9):
            i += 1
        toks.append(data[s:i])
        i += 1
    px = i
    n = 0
    for y in range(int(h * 0.80), h):
        for x in range(int(w * 0.45), w, 2):
            k = px + (y * w + x) * 3
            if data[k] > 200 and data[k + 1] > 200 and data[k + 2] < 120:
                n += 1
    return n


def main():
    stage("boot")
    vm = VM()
    vm.feed(3000)
    vm.expect("Language", 8000)
    for s in ("1", "SBOX", "admin", ""):
        vm.serial_cmd(s, 600)
    vm.expect("Enjoy", 8000)
    vm.s.sendall(b"\x1b")
    vm.feed(600)
    vm.expect("User:", 8000)
    vm.serial_cmd("admin", 600)
    vm.expect("Password", 3000)
    vm.serial_cmd("", 800)
    check("login via serial", vm.expect(">", 5000))

    # 1) NumLock 打开 -> 小键盘出数字
    stage("numlock on")
    vm.buf = ""
    vm.key("num_lock")
    vm.keys(["kp_1", "kp_2", "kp_3"])
    vm.key("ret")
    check("numlock keypad digits", vm.expect("123", 4000), vm.buf[-200:])

    # 2) NumLock 关闭 -> 小键盘 7 变成 Home (输入 z 后应插到行首)
    stage("numlock off")
    vm.buf = ""
    vm.key("num_lock")
    vm.keys(["4", "5"])
    vm.key("kp_7")
    vm.keys(["z"])
    check("numlock off -> Home key", vm.expect("z45", 4000), vm.buf[-200:])
    vm.key("ctrl-c")
    vm.feed(300)

    # 3) CapsLock -> 大写
    stage("capslock")
    vm.buf = ""
    vm.key("caps_lock")
    vm.keys(["k", "b", "d"])
    check("capslock uppercase", vm.expect("KBD", 4000), vm.buf[-200:])
    vm.key("caps_lock")
    vm.keys(["backspace", "backspace", "backspace", "backspace"])

    # 4) Ctrl+C 取消当前行
    stage("ctrl+c")
    vm.buf = ""
    vm.keys(["x", "y", "z"])
    vm.key("ctrl-c")
    check("ctrl+c cancels line", vm.expect("^C", 4000), vm.buf[-200:])

    # 5) Ctrl+Up 滚动提示 (截屏查黄色提示条)
    stage("ctrl+up")
    vm.keys(["h", "e", "l", "p", "ret"], 200)
    vm.feed(1200)
    vm.keys(["ctrl-up", "ctrl-up", "ctrl-up"])
    vm.feed(400)
    ppm = os.path.join(TMP, "kbd_ppm", "scroll.ppm")
    os.makedirs(os.path.dirname(ppm), exist_ok=True)
    vm.dump(ppm.replace("\\", "/"))
    if os.path.exists(ppm):
        with open(ppm, "rb") as f:
            head = f.read(64)
        dims = head.split(b"\n")[1].split()
        n = yellow_pixels(ppm, (int(dims[0]), int(dims[1])))
        check("ctrl+up scroll indicator", n > 20, f"yellow={n}")
    else:
        check("ctrl+up scroll indicator", False, "no screendump")

    # 6) Ctrl+Alt+Del 重启
    stage("ctrl+alt+del")
    vm.buf = ""
    vm.m.sendall(b"sendkey ctrl-alt-delete\n")
    time.sleep(0.3)
    try:
        vm.m.recv(4096)
    except Exception:
        pass
    check("ctrl+alt+del reboots", vm.expect("[boot] COM1", 15000),
          vm.buf[-200:])

    vm.close()
    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
