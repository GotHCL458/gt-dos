#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""C GUI 库单元测试: 编译/运行 \SDK\GUI_UT.C, 核对返回值、绘制像素与失败路径."""
import os, shutil, socket, subprocess, sys, time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun_lib"
SPORT = 5790
MPORT = 5791
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
                "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT); self.m = self._conn(MPORT); self.buf = ""

    @staticmethod
    def _conn(port, t=8.0):
        end = time.time() + t
        while time.time() < end:
            try: return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError: time.sleep(0.02)
        raise RuntimeError(port)

    def feed(self, ms=500):
        self.s.settimeout(0.2); end = time.time() + ms / 1000
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d: self.buf += d.decode("utf-8", "replace")
            except socket.timeout: pass
            except OSError: break

    def cmd(self, line, wait_ms=600):
        try: self.s.sendall((line + "\r").encode())
        except OSError: pass
        self.feed(wait_ms)

    def expect(self, needle, timeout_ms=8000):
        end = time.time() + timeout_ms / 1000; low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower(): return True
            self.feed(300)
        return False

    def mon(self, line, wait_ms=400):
        try: self.m.sendall((line + "\r\n").encode())
        except OSError: pass
        time.sleep(wait_ms / 1000)

    def shot(self, path):
        try: os.remove(path)
        except OSError: pass
        self.mon(f"screendump {path}", 700)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) >= 2359312:
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
    idx = 2; vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx:idx+1].isspace(): idx += 1
        if idx < len(data) and data[idx:idx+1] == b"#":
            while idx < len(data) and data[idx] != 10: idx += 1
            continue
        s = idx
        while idx < len(data) and data[idx:idx+1].isdigit(): idx += 1
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


def region_count(img, x0, y0, x1, y1, rgb):
    w, h, pix = img
    r, g, b = rgb
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            if pix[i] == r and pix[i + 1] == g and pix[i + 2] == b:
                n += 1
    return n


os.makedirs(TMP, exist_ok=True)
shutil.copy(os.path.join(SRC, "gt-dos-hd.img"), os.path.join(TMP, "hd.img"))
vm = VM(); vm.feed(3000)
vm.expect("Language", 6000)
vm.cmd("1"); vm.cmd("SBOX"); vm.cmd("admin"); vm.cmd("")
vm.expect("Enjoy GT-DOS", 6000)
vm.s.sendall(b"\x1b"); vm.feed(500)
vm.expect("User:", 8000)
vm.cmd("admin"); vm.expect("Password", 3000); vm.cmd("")
check("login", vm.expect(">", 5000))
check("default desktop", "[gui] desktop on" in vm.buf.lower(), vm.buf[-300:])

# 编译 GUI 单元测试程序
vm.buf = ""
vm.cmd("tc \\sdk\\gui_ut.c", 5000)
check("gui_ut compiles", vm.expect("Compiled ", 8000), vm.buf[-600:])

# 运行: 自检返回值 + 固定图案
vm.buf = ""
vm.cmd("run gui_ut.gtp", 2000)
check("gfx_info works", vm.expect("UT info ", 6000) and vm.expect("cw=", 3000),
      vm.buf[-500:])
check("gt_hit logic", vm.expect("UT hit 1001", 4000), vm.buf[-400:])
check("gui_begin works", vm.expect("UT drawn rc=0", 6000), vm.buf[-400:])

time.sleep(0.4)
img = vm.shot(os.path.join(SRC, "gui_ut.ppm"))
check("screendump canvas", img is not None)
if img:
    w, h, px = img
    print(f"       canvas {w}x{h}")
    check("resolution 1024x768", w == 1024 and h == 768, f"{w}x{h}")

    def pix(x, y):
        i = (y * w + x) * 3
        return (px[i], px[i + 1], px[i + 2])

    check("gfx_clear background", pix(10, 10) == (0x10, 0x1a, 0x30),
          str(pix(10, 10)))
    check("gt_panel title", pix(60, 50) == (0x1f, 0x4e, 0x8a), str(pix(60, 50)))
    check("gt_panel body", pix(60, 120) == (0x1b, 0x24, 0x34), str(pix(60, 120)))
    check("gt_panel border", pix(40, 120) == (0x9a, 0xa8, 0xc0),
          str(pix(40, 120)))
    check("gt_button normal", pix(370, 45) == (0x2f, 0x5a, 0x8a),
          str(pix(370, 45)))
    check("gt_button hot", pix(370, 85) == (0x3f, 0x6f, 0xae),
          str(pix(370, 85)))
    check("gt_checkbox on", pix(365, 127) == (0x66, 0xcc, 0xff),
          str(pix(365, 127)))
    check("gt_checkbox off", pix(365, 157) == (0x0e, 0x16, 0x22),
          str(pix(365, 157)))
    check("gt_radio on", pix(485, 47) == (0x66, 0xcc, 0xff),
          str(pix(485, 47)))
    check("gt_progress filled", pix(490, 98) == (0x4f, 0x9f, 0xdf),
          str(pix(490, 98)))
    check("gt_progress empty", pix(660, 98) == (0x0e, 0x16, 0x22),
          str(pix(660, 98)))
    check("gfx_fill red", pix(60, 230) == (0xff, 0, 0), str(pix(60, 230)))
    check("gfx_frame edge", pix(40, 260) == (0, 0xff, 0), str(pix(40, 260)))
    check("gfx_frame hollow", pix(60, 260) == (0x10, 0x1a, 0x30),
          str(pix(60, 260)))
    check("gfx_text pixels", region_count(img, 118, 218, 152, 240,
                                          (0xff, 0xff, 0x00)) > 2)

# 程序结束应恢复桌面
check("gui_end restores desktop", vm.expect("UT done", 9000), vm.buf[-300:])
time.sleep(0.4)
img2 = vm.shot(os.path.join(SRC, "gui_ut_done.ppm"))
check("screendump desktop", img2 is not None)
if img2:
    _, _, p2 = img2
    c = (p2[(95 * 1024 + 200) * 3], p2[(95 * 1024 + 200) * 3 + 1],
         p2[(95 * 1024 + 200) * 3 + 2])
    check("desktop window restored",
          c in {(0x1f, 0x4e, 0x8a), (0x2a, 0x6a, 0xb0)}, f"pixel={c}")

# 失败路径: 文本模式下 gui_begin 必须失败
vm.buf = ""
vm.cmd("textmode", 1200)
check("enter text mode", vm.expect("[gui] text mode", 6000), vm.buf[-300:])
vm.buf = ""
vm.cmd("run gui_ut.gtp", 3000)
check("gui_begin fails without desktop", vm.expect("UT NOGUI", 6000),
      vm.buf[-400:])
vm.buf = ""
vm.cmd("gui", 1200)
check("desktop re-entered", vm.expect("[gui] desktop on", 6000), vm.buf[-300:])

print("RESULT:", "ALL PASS" if not FAIL else f"{len(FAIL)} FAILED -> {FAIL}")
vm.close()
sys.exit(0 if not FAIL else 1)
