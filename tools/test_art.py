#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ICO / PNG / SVG 渲染 + TC 程序从宿主 HTTP 服务器下载 的端到端测试.

流程:
  1) 宿主起 HTTP 服务, 提供 icon.ico / image.png / logo.svg;
  2) 来宾启动 -> GUI OOBE/登录 -> 桌面;
  3) PUT DL.CFG 写入服务器地址, TC 编译 \\SDK\\DL.C, RUN 下载三个素材;
  4) SHOW 分别查看 ico/png/svg, 截屏校验中心像素.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun_art"
SPORT = 5780 + (os.getpid() % 40)
MPORT = SPORT + 1
HTTP_PORT = 26000 + (os.getpid() % 1000)
FAIL = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" +
          (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAIL.append(name)


# ------------------------------------------------------------ 测试素材
def make_ico():
    """32x32 32bpp BMP 条目; 中心绿色方块, RGB(60,200,20)."""
    W = H = 32
    rows = []
    for y in range(H):
        row = bytearray(W * 4)
        for x in range(W):
            b = (x * 8) & 255
            g = (y * 8) & 255
            r = 128
            a = 255
            if 10 <= x < 22 and 10 <= y < 22:
                b, g, r, a = 20, 200, 60, 255
            if x == W // 2 and y == H // 2:
                b, g, r, a = 20, 200, 60, 255
            o = x * 4
            row[o:o + 4] = bytes((b, g, r, a))
        rows.append(bytes(row))
    xor = b"".join(reversed(rows))                     # 底向上
    maskrow = ((W + 31) // 32) * 4
    mask = b"\x00" * (maskrow * H)
    dib = (struct.pack("<IiiHHIIiiII", 40, W, H * 2, 1, 32, 0,
                       len(xor) + len(mask), 0, 0, 0, 0) + xor + mask)
    icondir = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", W, H, 0, 0, 1, 32, len(dib), 6 + 16)
    return icondir + entry + dib


SVG = b"""<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <rect x="0" y="0" width="200" height="200" fill="#101820"/>
  <rect x="16" y="16" width="168" height="168" fill="#1f4e8a" stroke="#66ccff" stroke-width="6"/>
  <path d="M20 180 L100 30 L180 180 Z" fill="#20c060"/>
  <circle cx="100" cy="100" r="46" fill="#ff2080"/>
</svg>
"""


def http_server(sock, files):
    while True:
        try:
            c, _ = sock.accept()
        except OSError:
            return
        try:
            data = c.recv(2048)
            path = "/"
            if data.startswith(b"GET "):
                parts = data.split(b" ")
                if len(parts) >= 2:
                    path = parts[1].decode("latin1")
            body = files.get(path, b"")
            hdr = (b"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                   b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body))
            c.sendall(hdr + body)
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass


# ------------------------------------------------------------ VM 辅助
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


def px(img, x, y):
    w, h, pix = img
    i = (y * w + x) * 3
    return (pix[i], pix[i + 1], pix[i + 2])


def main():
    os.makedirs(TMP, exist_ok=True)
    shutil.copy(os.path.join(SRC, "gt-dos-hd.img"), os.path.join(TMP, "hd.img"))

    png_bytes = open(os.path.join(SRC, "sample.png"), "rb").read()
    files = {"/icon.ico": make_ico(), "/image.png": png_bytes, "/logo.svg": SVG}

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", HTTP_PORT))
    srv.listen(8)
    threading.Thread(target=http_server, args=(srv, files), daemon=True).start()
    print(f"       http server on 127.0.0.1:{HTTP_PORT} "
          f"(ico={len(files['/icon.ico'])}, png={len(png_bytes)}, "
          f"svg={len(SVG)})")

    vm = VM()
    vm.feed(3000)
    check("OOBE starts", vm.expect("Language", 9000))
    vm.cmd("1")
    vm.cmd("SBOX")
    vm.cmd("admin")
    vm.cmd("")
    check("OOBE done", vm.expect("Enjoy GT-DOS", 8000), vm.buf[-300:])
    vm.key(b"\x1b", 500)
    check("login screen", vm.expect("User:", 8000), vm.buf[-300:])
    vm.cmd("admin")
    vm.expect("Password", 3000)
    vm.cmd("")
    check("login ok", vm.expect(">", 5000), vm.buf[-300:])

    # ---- 配置服务器地址并通过 TC 程序下载 ----
    vm.cmd(f"put dl.cfg 10.0.2.2:{HTTP_PORT}", 800)
    vm.buf = ""
    vm.cmd("tc \\sdk\\dl.c", 7000)
    check("dl compiles", vm.expect("Compiled", 9000), vm.buf[-300:])
    vm.buf = ""
    vm.cmd("run dl.gtp", 10000)
    check("downloads ico/png/svg", vm.expect("DL: all downloaded", 15000),
          vm.buf[-400:])

    vm.buf = ""
    vm.cmd("dir", 1200)
    ok_dir = ("icon.ico" in vm.buf.lower() and "image.png" in vm.buf.lower()
              and "logo.svg" in vm.buf.lower())
    check("downloaded files on disk", ok_dir, vm.buf[-300:])

    # ---- SHOW 并截图校验 ----
    def show_shot(name, out):
        vm.buf = ""
        vm.cmd(f"show {name}", 1500)
        got = vm.expect("Press any key", 6000)
        img = vm.shot(os.path.join(SRC, out))
        vm.key(b"x", 600)
        return got, img

    got, img = show_shot("icon.ico", "art_ico.ppm")
    check("show ico", got and img is not None, vm.buf[-200:])
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       ico center {c}")
        check("ico center pixel", c == (60, 200, 20), f"got {c}")

    got, img = show_shot("logo.svg", "art_svg.ppm")
    check("show svg", got and img is not None, vm.buf[-200:])
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       svg center {c}")
        check("svg center pixel", c == (255, 32, 128), f"got {c}")
        # 左上角背景色 #101820 (在外层 rect 内)
        c2 = px(img, w // 8, h // 8)
        print(f"       svg bg {c2}")
        check("svg background rect", c2 == (0x10, 0x18, 0x20), f"got {c2}")

    got, img = show_shot("image.png", "art_png.ppm")
    check("show png", got and img is not None, vm.buf[-200:])
    if img:
        w, h, _ = img
        c = px(img, w // 2, h // 2)
        print(f"       png center {c}")
        check("png center pixel", c == (255, 32, 200), f"got {c}")

    print("RESULT:", "ALL PASS" if not FAIL else f"{len(FAIL)} FAILED -> {FAIL}")
    try:
        srv.close()
    except OSError:
        pass
    vm.close()
    sys.exit(0 if not FAIL else 1)


if __name__ == "__main__":
    main()
