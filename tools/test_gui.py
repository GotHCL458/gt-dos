#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GUI 桌面一次性验证: 进入桌面、截屏、鼠标移动/点击、返回文本."""
import os, shutil, socket, subprocess, sys, time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 5740
MPORT = 5741
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
        for _ in range(10):
            if os.path.exists(path): return read_ppm(path)
            time.sleep(0.2)
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
    for i in range(0, w * h * 3, 3 * 7):     # 采样
        if pix[i] == r and pix[i + 1] == g and pix[i + 2] == b:
            n += 1
    return n * 7

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

vm.buf = ""
vm.cmd("desktop", 1500)
check("enter desktop", vm.expect("[gui] desktop on", 8000), vm.buf[-400:])
time.sleep(0.5)
img1 = vm.shot(os.path.join(SRC, "gui1.ppm"))
check("screendump", img1 is not None)
if img1:
    w, h, _ = img1
    print(f"       screen {w}x{h}")
    check("window title drawn", color_count(img1, (0x1f, 0x4e, 0x8a)) > 500)
    check("taskbar drawn", color_count(img1, (0x14, 0x1c, 0x2a)) > 500)

# 键盘仍可用: 在桌面里执行命令
vm.buf = ""
vm.cmd("ver", 1200)
check("keyboard inside GUI", vm.expect("GT-DOS", 6000), vm.buf[-300:])

# 窗口拖动: 指针在客户区中心 -> 标题栏 -> 按下 -> 移动 -> 松开
drag_ok = False
img3 = None
if img1:
    w, h, p1 = img1
    k = 2 if w >= 1280 else 1
    def move_guest(gx, gy):
        dx, dy = int(gx / k), int(gy / k)
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
            vm.mon(f"mouse_move {sx} {sy}", 60)
            dx -= sx; dy -= sy
    tx, ty = int(w * 0.2), int(h * 0.12)          # 标题栏上的点
    move_guest(tx - w // 2, ty - h // 2)
    vm.mon("mouse_button 1", 300)
    gdx, gdy = int(w * 0.08), int(h * 0.04)        # 拖动位移
    move_guest(gdx, gdy)
    vm.mon("mouse_button 0", 500)
    time.sleep(0.4)
    img3 = vm.shot(os.path.join(SRC, "gui3.ppm"))
    check("screendump after drag", img3 is not None)
    if img3:
        _, _, p3 = img3
        def px(p, x, y):
            i = (y * w + x) * 3
            return (p[i], p[i + 1], p[i + 2])
        titles = {(0x2a, 0x6a, 0xb0), (0x1f, 0x4e, 0x8a)}
        old = px(p3, tx, ty)
        new = px(p3, tx + gdx + 40, ty + gdy)   # 避开指针本身
        print(f"       drag old={old} new={new}")
        check("window moved by drag", new in titles and old not in titles,
              f"old={old} new={new}")
        drag_ok = True

# 鼠标移动: 连续相对移动后截屏, 与第一张比较
for _ in range(8):
    vm.mon("mouse_move 40 30", 120)
time.sleep(0.4)
img2 = vm.shot(os.path.join(SRC, "gui2.ppm"))
check("screendump after mouse", img2 is not None)
if img3 and img2:
    _, _, a = img3; _, _, b = img2
    diff = sum(1 for i in range(0, min(len(a), len(b)), 3 * 11)
               if a[i:i+3] != b[i:i+3])
    print(f"       mouse diff samples {diff}")
    check("mouse moves pointer", diff > 0)

# 点击任务栏 TEXT 按钮 (贴住右下角): 移到屏幕角落即可
for _ in range(40):
    vm.mon("mouse_move 120 120", 30)
vm.mon("mouse_button 1", 350)
vm.mon("mouse_button 0", 450)
check("click TEXT exits GUI", vm.expect("[gui] text mode", 6000), vm.buf[-400:])

# 再次进入并用命令返回, 验证可重复切换
vm.buf = ""
vm.cmd("gui", 1200)
check("re-enter desktop", vm.expect("[gui] desktop on", 6000), vm.buf[-400:])

# ---- 新控件: 开始菜单 / 主题 / 关于 / 最大化 ----
# gui_enter 把指针放到客户区中心; 这里维护绝对坐标并按相对位移移动.
pos = [512, 380]
def goto(gx, gy):
    dx, dy = int(gx) - pos[0], int(gy) - pos[1]
    while dx or dy:
        sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
        vm.mon(f"mouse_move {sx} {sy}", 40)
        dx -= sx; dy -= sy
        pos[0] += sx; pos[1] += sy
def click():
    # QEMU 监视器的 mouse_button 可能随下一次移动才送达客户机;
    # 按下/松开前后各抖动 1px, 强制产生携带按键状态的 PS/2 数据包.
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    vm.mon("mouse_button 1", 250)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    vm.mon("mouse_button 0", 300)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)

# 开始菜单 (任务栏左下 GT-DOS 按钮)
goto(44, 755); click(); time.sleep(0.3)
img_menu = vm.shot(os.path.join(SRC, "gui_menu.ppm"))
check("screendump menu", img_menu is not None)
if img_menu:
    check("start menu drawn", color_count(img_menu, (0x22, 0x3a, 0x58)) > 200)

# Theme: 换壁纸配色 (菜单关闭 + 壁纸颜色变化)
theme_before = None
if img_menu:
    _, _, pm = img_menu
    theme_before = (pm[(300 * 1024 + 960) * 3], pm[(300 * 1024 + 960) * 3 + 1],
                    pm[(300 * 1024 + 960) * 3 + 2])
goto(68, 678); click(); time.sleep(0.3)
img_theme = vm.shot(os.path.join(SRC, "gui_theme.ppm"))
check("screendump theme", img_theme is not None)
if img_theme and theme_before:
    _, _, pt = img_theme
    theme_after = (pt[(300 * 1024 + 960) * 3], pt[(300 * 1024 + 960) * 3 + 1],
                   pt[(300 * 1024 + 960) * 3 + 2])
    print(f"       theme {theme_before} -> {theme_after}")
    check("theme switched wallpaper", theme_after != theme_before)

# About 对话框 + OK 关闭
goto(44, 755); click()
goto(68, 702); click(); time.sleep(0.3)
img_about = vm.shot(os.path.join(SRC, "gui_about.ppm"))
check("screendump about", img_about is not None)
if img_about:
    _, _, pa = img_about
    # 对话框标题栏 (右侧空白处) 应为 C_TITLE; 无对话框时这里是控制台黑底
    c = (pa[(300 * 1024 + 680) * 3], pa[(300 * 1024 + 680) * 3 + 1],
         pa[(300 * 1024 + 680) * 3 + 2])
    print(f"       about title pixel {c}")
    check("about dialog drawn", c == (0x1f, 0x4e, 0x8a))
goto(660, 456); click(); time.sleep(0.2)

# 最大化: 标题栏应铺到屏幕左侧 (x=10,y=8)
goto(863, 95); click(); time.sleep(0.4)
img_max = vm.shot(os.path.join(SRC, "gui_max.ppm"))
check("screendump maximize", img_max is not None)
if img_max:
    _, _, pz = img_max
    c = (pz[(8 * 1024 + 10) * 3], pz[(8 * 1024 + 10) * 3 + 1],
         pz[(8 * 1024 + 10) * 3 + 2])
    print(f"       maximized title pixel {c}")
    check("window maximized", c in {(0x1f, 0x4e, 0x8a), (0x2a, 0x6a, 0xb0)})

# 还原 (最大化后按钮移到最右侧)
goto(991, 12); click(); time.sleep(0.4)
img_rest = vm.shot(os.path.join(SRC, "gui_restore.ppm"))
check("screendump restore", img_rest is not None)
if img_rest:
    _, _, pr = img_rest
    c = (pr[(8 * 1024 + 10) * 3], pr[(8 * 1024 + 10) * 3 + 1],
         pr[(8 * 1024 + 10) * 3 + 2])
    check("window restored", c not in {(0x1f, 0x4e, 0x8a), (0x2a, 0x6a, 0xb0)})

vm.buf = ""
vm.cmd("textmode", 1200)
check("textmode command", vm.expect("[gui] text mode", 6000), vm.buf[-400:])
vm.buf = ""
vm.cmd("ver", 1000)
check("shell alive after GUI", vm.expect("GT-DOS", 6000), vm.buf[-300:])

print("RESULT:", "ALL PASS" if not FAIL else f"{len(FAIL)} FAILED -> {FAIL}")
vm.close()
sys.exit(0 if not FAIL else 1)
