#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GUI 登录/OOBE + 完整 PNG 渲染验证.

- 走图形化 OOBE/登录 (串口握手仍可用);
- PNGINFO 解析各颜色类型/位深/隔行;
- SHOW 渲染 PNG 并截屏校验像素 (RGBA / 调色板+tRNS / Adam7 / 16 位)."""
import os
import shutil
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun_png"
SPORT = 5760
MPORT = 5761
FAIL = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" +
          (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAIL.append(name)


class VM:
    def __init__(self):
        args = [QEMU, "-m", "32", "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
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

    def cmd(self, line, wait_ms=600):
        try:
            self.s.sendall((line + "\r").encode())
        except OSError:
            pass
        self.feed(wait_ms)

    def key(self, data, wait_ms=500):
        try:
            self.s.sendall(data)
        except OSError:
            pass
        self.feed(wait_ms)

    def expect(self, needle, timeout_ms=8000):
        end = time.time() + timeout_ms / 1000
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(300)
        return False

    def mon(self, line, wait_ms=400):
        try:
            self.m.sendall((line + "\r\n").encode())
        except OSError:
            pass
        time.sleep(wait_ms / 1000)

    def shot(self, path):
        try:
            os.remove(path)
        except OSError:
            pass
        self.mon(f"screendump {path}", 700)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                time.sleep(0.15)
                img = read_ppm(path)
                if img:
                    return img
            time.sleep(0.1)
        return None

    def close(self):
        if self.p.poll() is None:
            self.p.kill()


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        return None
    idx = 2
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx:idx + 1].isspace():
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] != 10:
                idx += 1
            continue
        s = idx
        while idx < len(data) and data[idx:idx + 1].isdigit():
            idx += 1
        vals.append(int(data[s:idx]))
    idx += 1
    w, h, maxv = vals
    pix = data[idx:]
    if maxv != 255 or len(pix) < w * h * 3:
        return None
    return w, h, pix


def color_count(img, rgb):
    w, h, pix = img
    r, g, b = rgb
    n = 0
    for i in range(0, w * h * 3, 3 * 7):
        if pix[i] == r and pix[i + 1] == g and pix[i + 2] == b:
            n += 1
    return n * 7


def px(img, x, y):
    w, h, pix = img
    i = (y * w + x) * 3
    return (pix[i], pix[i + 1], pix[i + 2])


def fit_origin(sw, sh, iw, ih):
    s = min(sw // iw, sh // ih)
    if s < 1:
        s = 1
    if s > 8:
        s = 8
    return s, (sw - iw * s) // 2, (sh - ih * s) // 2


def main():
    os.makedirs(TMP, exist_ok=True)
    shutil.copy(os.path.join(SRC, "gt-dos-hd.img"), os.path.join(TMP, "hd.img"))

    vm = VM()
    vm.feed(3000)

    # ---- 图形化 OOBE ----
    check("OOBE starts", vm.expect("Language", 9000))
    img_oobe = vm.shot(os.path.join(SRC, "png_oobe.ppm"))
    check("OOBE screendump", img_oobe is not None)
    if img_oobe:
        check("OOBE is GUI (title bar)", color_count(img_oobe, (0x1f, 0x4e, 0x8a)) > 100,
              "no panel title pixels")

    vm.cmd("1")            # English
    vm.cmd("SBOX")         # host name
    vm.cmd("admin")        # admin user
    vm.cmd("")             # no password
    check("OOBE done", vm.expect("Enjoy GT-DOS", 8000), vm.buf[-300:])

    # ---- 图形化登录 ----
    vm.key(b"\x1b", 500)
    check("login screen", vm.expect("User:", 8000), vm.buf[-300:])
    img_login = vm.shot(os.path.join(SRC, "png_login.ppm"))
    check("login screendump", img_login is not None)
    if img_login:
        check("login is GUI (title bar)",
              color_count(img_login, (0x1f, 0x4e, 0x8a)) > 100)
    vm.cmd("admin")
    vm.expect("Password", 3000)
    vm.cmd("")
    check("login ok", vm.expect(">", 5000), vm.buf[-300:])

    # 默认自动进入图形桌面
    if "[gui] desktop on" not in vm.buf.lower():
        vm.buf = ""
        vm.cmd("desktop", 1500)
        check("desktop on", vm.expect("[gui] desktop on", 8000), vm.buf[-300:])
    else:
        check("desktop on", True)

    # ---- PNGINFO: 头部字段 ----
    vm.buf = ""
    vm.cmd("pnginfo sample.png", 1200)
    ok_info = vm.expect("128 x 128", 5000)
    check("pnginfo rgba", ok_info and "rgba" in vm.buf.lower(), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("pnginfo adam7.png", 1200)
    check("pnginfo adam7", vm.expect("interlace=1", 5000), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("pnginfo pal8.png", 1200)
    check("pnginfo palette", vm.expect("palette", 5000), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("pnginfo rgb16.png", 1200)
    check("pnginfo 16-bit", vm.expect("depth=16", 5000), vm.buf[-300:])

    # ---- SHOW: 渲染 + 像素校验 ----
    def show_and_shot(name, out):
        vm.buf = ""
        vm.cmd(f"show {name}", 1500)
        got = vm.expect("Press any key", 6000)
        print("       show:", " | ".join(
            l.strip() for l in vm.buf.splitlines() if l.strip())[-160:])
        img = vm.shot(os.path.join(SRC, out))
        vm.key(b"x", 600)
        return got, img

    got, img = show_and_shot("sample.png", "png_show_sample.ppm")
    check("show sample", got and img is not None)
    if img:
        w, h, _ = img
        s, ox, oy = fit_origin(w, h, 128, 128)
        c = px(img, w // 2, h // 2)
        print(f"       sample center {c}  origin {ox},{oy} s={s}")
        check("sample center pixel", c == (255, 32, 200), f"got {c}")
        corner = px(img, ox + 2, oy + 2)
        print(f"       sample corner {corner}")
        check("sample alpha transparent", corner == (0x10, 0x18, 0x20),
              f"got {corner}")

    got, img = show_and_shot("pal8.png", "png_show_pal.ppm")
    check("show palette", got and img is not None)
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       palette center {c}")
        check("palette center pixel", c == (0, 128, 255), f"got {c}")

    got, img = show_and_shot("adam7.png", "png_show_adam7.ppm")
    check("show adam7", got and img is not None)
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       adam7 center {c}")
        check("adam7 center pixel", c == (250, 10, 120), f"got {c}")

    got, img = show_and_shot("rgb16.png", "png_show_rgb16.ppm")
    check("show rgb16", got and img is not None)
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       rgb16 center {c}")
        check("rgb16 center pixel", c == (3, 195, 117), f"got {c}")

    got, img = show_and_shot("gray.png", "png_show_gray.ppm")
    check("show gray", got and img is not None)
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       gray center {c}")
        check("gray center pixel", c == (200, 200, 200), f"got {c}")

    print("RESULT:", "ALL PASS" if not FAIL else f"{len(FAIL)} FAILED -> {FAIL}")
    vm.close()
    sys.exit(0 if not FAIL else 1)


if __name__ == "__main__":
    main()
