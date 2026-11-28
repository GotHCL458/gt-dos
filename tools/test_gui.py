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
        # QEMU 是异步写 PPM 的: 等文件达到整屏大小后再解析, 避免读到截断帧
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

# 默认配置下登录后自动进入图形桌面; 若未自动进入则用 desktop 命令
auto_gui = "[gui] desktop on" in vm.buf.lower()
check("default GUI at boot", auto_gui, vm.buf[-400:])
if not auto_gui:
    vm.buf = ""
    vm.cmd("desktop", 1500)
    check("enter desktop", vm.expect("[gui] desktop on", 8000), vm.buf[-400:])
time.sleep(0.5)
img1 = vm.shot(os.path.join(SRC, "gui1.ppm"))
check("screendump", img1 is not None)
if img1:
    w, h, _ = img1
    print(f"       screen {w}x{h}")
    check("window title drawn", color_count(img1, (0x2a, 0x2e, 0x36)) > 500)
    check("taskbar drawn", color_count(img1, (0x1b, 0x1e, 0x24)) > 500)

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
        titles = {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)}
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

def rclick():
    # QEMU 监视器: mouse_button 2 = 右键 (PS/2 bit1), 4 = 中键
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    vm.mon("mouse_button 2", 220)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    vm.mon("mouse_button 0", 250)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)

def drag(fx, fy, tx, ty):
    goto(fx, fy)
    vm.mon("mouse_button 1", 220)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    goto(tx, ty)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
    vm.mon("mouse_button 0", 280)
    vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)

# 开始菜单 (任务栏左下 GT-DOS 按钮)
goto(44, 755); click(); time.sleep(0.3)
img_menu = vm.shot(os.path.join(SRC, "gui_menu.ppm"))
check("screendump menu", img_menu is not None)
if img_menu:
    check("start menu drawn", color_count(img_menu, (0x2b, 0x30, 0x38)) > 5000)

# 悬停高亮: 移到 Theme 项 (6 项菜单的), 该项背景应变亮
goto(68, 630); time.sleep(0.3)
img_hover = vm.shot(os.path.join(SRC, "gui_menu_hover.ppm"))
check("screendump menu hover", img_hover is not None)
if img_hover:
    _, _, ph = img_hover
    c = (ph[(626 * 1024 + 110) * 3], ph[(626 * 1024 + 110) * 3 + 1],
         ph[(626 * 1024 + 110) * 3 + 2])
    print(f"       hover pixel {c}")
    check("menu hover highlight", c == (0x3d, 0xae, 0xe9), f"pixel={c}")

# Theme: 换壁纸配色 (菜单关闭 + 壁纸颜色变化)
theme_before = None
if img_menu:
    _, _, pm = img_menu
    theme_before = (pm[(300 * 1024 + 960) * 3], pm[(300 * 1024 + 960) * 3 + 1],
                    pm[(300 * 1024 + 960) * 3 + 2])
goto(68, 630); click(); time.sleep(0.3)
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
goto(68, 654); click(); time.sleep(0.3)
img_about = vm.shot(os.path.join(SRC, "gui_about.ppm"))
check("screendump about", img_about is not None)
if img_about:
    _, _, pa = img_about
    # 对话框标题栏 (右侧空白处) 应为 C_TITLE; 无对话框时这里是控制台黑底
    c = (pa[(300 * 1024 + 680) * 3], pa[(300 * 1024 + 680) * 3 + 1],
         pa[(300 * 1024 + 680) * 3 + 2])
    print(f"       about title pixel {c}")
    check("about dialog drawn", c == (0x2a, 0x2e, 0x36))
    # 控制台输出会重绘视口, 浮层必须仍在最上层
    vm.buf = ""
    vm.cmd("ver", 1000)
    img_about2 = vm.shot(os.path.join(SRC, "gui_about_after.ppm"))
    check("screendump about after output", img_about2 is not None)
    if img_about2:
        _, _, pb = img_about2
        c2 = (pb[(300 * 1024 + 680) * 3], pb[(300 * 1024 + 680) * 3 + 1],
              pb[(300 * 1024 + 680) * 3 + 2])
        check("about stays above console output", c2 == (0x2a, 0x2e, 0x36),
              f"pixel={c2}")
goto(660, 456); click(); time.sleep(0.2)

# ---- 设置对话框: 主题按钮 / 复选框 / 亮度滑块 ----
goto(44, 755); click()
goto(68, 678); click(); time.sleep(0.3)
img_set = vm.shot(os.path.join(SRC, "gui_settings.ppm"))
check("screendump settings", img_set is not None)
wall0 = wall1 = None
if img_set:
    _, _, ps = img_set
    c = (ps[(298 * 1024 + 668) * 3], ps[(298 * 1024 + 668) * 3 + 1],
         ps[(298 * 1024 + 668) * 3 + 2])
    print(f"       settings title pixel {c}")
    check("settings dialog drawn", c == (0x2a, 0x2e, 0x36))
    wall0 = (ps[(300 * 1024 + 960) * 3], ps[(300 * 1024 + 960) * 3 + 1],
             ps[(300 * 1024 + 960) * 3 + 2])
    # 终端图标: 图标底板 (蓝) 应被真实图标图形覆盖, 底板边缘仍为蓝色
    ic = (ps[(96 * 1024 + 40) * 3], ps[(96 * 1024 + 40) * 3 + 1],
          ps[(96 * 1024 + 40) * 3 + 2])
    check("terminal icon rendered", ic == (0x2f, 0x6f, 0x9f), f"pixel={ic}")

# 主题 ">" 按钮: 壁纸变化
goto(660, 333); click(); time.sleep(0.3)
img_set2 = vm.shot(os.path.join(SRC, "gui_settings_theme.ppm"))
check("screendump settings theme", img_set2 is not None)
if img_set2 and wall0:
    _, _, ps2 = img_set2
    wall1 = (ps2[(300 * 1024 + 960) * 3], ps2[(300 * 1024 + 960) * 3 + 1],
             ps2[(300 * 1024 + 960) * 3 + 2])
    print(f"       settings theme {wall0} -> {wall1}")
    check("settings theme button works", wall1 != wall0)

# 亮度滑块: 拖到左侧变暗
goto(450, 414); click(); time.sleep(0.3)
img_set3 = vm.shot(os.path.join(SRC, "gui_settings_dim.ppm"))
check("screendump settings brightness", img_set3 is not None)
if img_set3 and wall1:
    _, _, ps3 = img_set3
    wall2 = (ps3[(300 * 1024 + 960) * 3], ps3[(300 * 1024 + 960) * 3 + 1],
             ps3[(300 * 1024 + 960) * 3 + 2])
    print(f"       brightness {wall1} -> {wall2}")
    check("brightness slider works", sum(wall2) < sum(wall1))

# 复选框: 隐藏桌面图标 (TERM 图标像素应变为壁纸)
goto(428, 362); click(); time.sleep(0.3)
img_noicon = vm.shot(os.path.join(SRC, "gui_settings_noicon.ppm"))
check("screendump icons off", img_noicon is not None)
if img_noicon:
    _, _, pn = img_noicon
    c = (pn[(96 * 1024 + 40) * 3], pn[(96 * 1024 + 40) * 3 + 1],
         pn[(96 * 1024 + 40) * 3 + 2])
    print(f"       icon pixel after hide {c}")
    check("desktop icons hidden", c != (0x2f, 0x6f, 0x9f))

# 恢复图标并关闭设置
goto(428, 362); click(); time.sleep(0.2)
goto(652, 456); click(); time.sleep(0.2)

# ---- 控件演示: 单选组 / 进度条 / 时钟开关 / 列表 ----
goto(44, 755); click()
goto(68, 702); click(); time.sleep(0.5)
img_wd = vm.shot(os.path.join(SRC, "gui_widgets.ppm"))
check("screendump widgets", img_wd is not None)
wall_wd = None
if img_wd:
    _, _, pw = img_wd
    c = (pw[(260 * 1024 + 650) * 3], pw[(260 * 1024 + 650) * 3 + 1],
         pw[(260 * 1024 + 650) * 3 + 2])
    print(f"       widgets title pixel {c}")
    check("widgets dialog drawn", c == (0x2a, 0x2e, 0x36))
    # 进度条已开始填充
    pc = (pw[(332 * 1024 + 368) * 3], pw[(332 * 1024 + 368) * 3 + 1],
          pw[(332 * 1024 + 368) * 3 + 2])
    check("widgets progress drawn", pc == (0x3d, 0xae, 0xe9), f"pixel={pc}")
    # 时钟开关默认打开 (绿色轨道)
    tc = (pw[(356 * 1024 + 632) * 3], pw[(356 * 1024 + 632) * 3 + 1],
          pw[(356 * 1024 + 632) * 3 + 2])
    check("widgets toggle on", tc == (0x2f, 0x8f, 0x5f), f"pixel={tc}")
    wall_wd = (pw[(300 * 1024 + 960) * 3], pw[(300 * 1024 + 960) * 3 + 1],
               pw[(300 * 1024 + 960) * 3 + 2])

# 单选 Green: 壁纸配色变化
goto(460, 288); click(); time.sleep(0.3)
img_wd2 = vm.shot(os.path.join(SRC, "gui_widgets_radio.ppm"))
check("screendump widgets radio", img_wd2 is not None)
if img_wd2 and wall_wd:
    _, _, pr2 = img_wd2
    w2 = (pr2[(300 * 1024 + 960) * 3], pr2[(300 * 1024 + 960) * 3 + 1],
          pr2[(300 * 1024 + 960) * 3 + 2])
    print(f"       widgets radio {wall_wd} -> {w2}")
    check("widgets radio works", w2 != wall_wd)

# 列表选择 Beta: 选中行高亮
goto(400, 406); click(); time.sleep(0.3)
img_wd3 = vm.shot(os.path.join(SRC, "gui_widgets_list.ppm"))
check("screendump widgets list", img_wd3 is not None)
if img_wd3:
    _, _, pl = img_wd3
    c = (pl[(400 * 1024 + 370) * 3], pl[(400 * 1024 + 370) * 3 + 1],
         pl[(400 * 1024 + 370) * 3 + 2])
    print(f"       list selected pixel {c}")
    check("widgets list selection", c == (0x3a, 0x3f, 0x46))
    check("taskbar clock visible",
          color_count(img_wd3, (0xcf, 0xe4, 0xff)) > 2)

# 时钟开关 -> 关闭: 轨道变灰
goto(644, 356); click(); time.sleep(0.35)
img_wtg = vm.shot(os.path.join(SRC, "gui_widgets_toggle.ppm"))
check("screendump widgets toggle off", img_wtg is not None)
if img_wtg:
    _, _, ptw = img_wtg
    tc2 = (ptw[(356 * 1024 + 654) * 3], ptw[(356 * 1024 + 654) * 3 + 1],
           ptw[(356 * 1024 + 654) * 3 + 2])
    print(f"       toggle off pixel {tc2}")
    check("widgets toggle off", tc2 == (0x3a, 0x46, 0x58), f"pixel={tc2}")
    # 任务栏时钟完全隐藏 (时钟文字色不再出现)
    check("taskbar clock hidden",
          color_count(img_wtg, (0xcf, 0xe4, 0xff)) == 0,
          f"clock_pixels={color_count(img_wtg, (0xcf, 0xe4, 0xff))}")

# 关闭控件演示
goto(636, 496); click(); time.sleep(0.2)

# ---- 任务栏窗口按钮: 点击最大化, 再点还原 ----
goto(138, 755); click(); time.sleep(0.4)
img_tb = vm.shot(os.path.join(SRC, "gui_taskbtn_max.ppm"))
check("screendump taskbar button", img_tb is not None)
if img_tb:
    _, _, ptb = img_tb
    c = (ptb[(8 * 1024 + 10) * 3], ptb[(8 * 1024 + 10) * 3 + 1],
         ptb[(8 * 1024 + 10) * 3 + 2])
    check("taskbar button maximizes",
          c in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)}, f"pixel={c}")
goto(138, 755); click(); time.sleep(0.4)

# 最大化: 标题栏应铺到屏幕左侧 (x=10,y=8)
goto(863, 95); click(); time.sleep(0.4)
img_max = vm.shot(os.path.join(SRC, "gui_max.ppm"))
check("screendump maximize", img_max is not None)
if img_max:
    _, _, pz = img_max
    c = (pz[(8 * 1024 + 10) * 3], pz[(8 * 1024 + 10) * 3 + 1],
         pz[(8 * 1024 + 10) * 3 + 2])
    print(f"       maximized title pixel {c}")
    check("window maximized", c in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)})

# 还原 (最大化后按钮移到最右侧)
goto(991, 12); click(); time.sleep(0.4)
img_rest = vm.shot(os.path.join(SRC, "gui_restore.ppm"))
check("screendump restore", img_rest is not None)
if img_rest:
    _, _, pr = img_rest
    c = (pr[(8 * 1024 + 10) * 3], pr[(8 * 1024 + 10) * 3 + 1],
         pr[(8 * 1024 + 10) * 3 + 2])
    check("window restored", c not in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)})

# ---- 缩放握把: 按住右下角拖动放大窗口 ----
goto(888, 651)
vm.mon("mouse_button 1", 250)
vm.mon("mouse_move 1 0", 60); vm.mon("mouse_move -1 0", 60)
time.sleep(0.3)
goto(948, 691)                    # 向右下拖 (+60,+40)
vm.mon("mouse_button 0", 300)
time.sleep(0.4)
img_rs = vm.shot(os.path.join(SRC, "gui_resize.ppm"))
check("screendump resize", img_rs is not None)
if img_rs:
    _, _, prs = img_rs
    # 放大后窗口右缘到达 x≈956, (900,95) 应落在标题栏 (右侧按钮之前)
    c = (prs[(85 * 1024 + 900) * 3], prs[(85 * 1024 + 900) * 3 + 1],
         prs[(85 * 1024 + 900) * 3 + 2])
    print(f"       resized title pixel {c}")
    check("window resized by grip",
          c in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)})

# ---- 最小化 / 还原 (Linux 风格图标按钮) ----
goto(900, 95); click(); time.sleep(0.6)
img_min = vm.shot(os.path.join(SRC, "gui_min.ppm"))
check("screendump minimize", img_min is not None)
if img_min:
    _, _, pm = img_min
    c = (pm[(8 * 1024 + 10) * 3], pm[(8 * 1024 + 10) * 3 + 1],
         pm[(8 * 1024 + 10) * 3 + 2])
    print(f"       minimize title pixel {c}")
    check("window minimized", c not in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)})
goto(138, 755); click(); time.sleep(0.6)
img_unmin = vm.shot(os.path.join(SRC, "gui_unmin.ppm"))
check("screendump unminimize", img_unmin is not None)
if img_unmin:
    _, _, pu = img_unmin
    c = (pu[(85 * 1024 + 900) * 3], pu[(85 * 1024 + 900) * 3 + 1],
         pu[(85 * 1024 + 900) * 3 + 2])
    print(f"       unminimize title pixel {c}")
    check("window restored from minimize",
          c in {(0x2a, 0x2e, 0x36), (0x32, 0x36, 0x3f)})

# ---- 桌面交互: 右键菜单 / 属性 / 双击运行 / 拖动 / 拖入窗口 ----
vm.buf = ""
goto(100, 700)
rclick()
check("right-click context menu", vm.expect("[gui] menu", 4000), vm.buf[-300:])
img_ctx = vm.shot(os.path.join(SRC, "gui_ctx.ppm"))
check("screendump context menu", img_ctx is not None)
if img_ctx:
    _, _, pc = img_ctx
    i = (686 * 1024 + 240) * 3
    c = (pc[i], pc[i + 1], pc[i + 2])
    print(f"       context pixel {c}")
    check("context menu drawn",
          c in {(0x2b, 0x30, 0x38), (0x3d, 0xae, 0xe9)}, f"pixel={c}")

vm.buf = ""
goto(110, 686); click(); time.sleep(0.3)
check("properties opened", vm.expect("[gui] properties", 4000), vm.buf[-300:])
img_prop = vm.shot(os.path.join(SRC, "gui_props.ppm"))
check("screendump properties", img_prop is not None)
if img_prop:
    _, _, pp = img_prop
    i = (288 * 1024 + 600) * 3
    c = (pp[i], pp[i + 1], pp[i + 2])
    print(f"       properties title pixel {c}")
    check("properties dialog drawn", c == (0x2a, 0x2e, 0x36))
goto(652, 464); click(); time.sleep(0.2)      # OK 关闭

# 双击 FILES 图标: 打开文件应用
vm.buf = ""
goto(32, 184); click(); click(); time.sleep(0.4)
check("files app opened", vm.expect("[gui] open files", 5000), vm.buf[-300:])
img_files = vm.shot(os.path.join(SRC, "gui_files.ppm"))
check("screendump files", img_files is not None)
if img_files:
    _, _, pf = img_files
    i = (250 * 1024 + 700) * 3
    c = (pf[i], pf[i + 1], pf[i + 2])
    print(f"       files title pixel {c}")
    check("files window drawn", c == (0x2a, 0x2e, 0x36))
    # SAMPLE.GTL 行左侧应显示内嵌图标 (WIDGETS 四色之一)
    gtl_cols = {(0x3d, 0xae, 0xe9), (0x7f, 0xe0, 0xa0),
                (0xff, 0xc0, 0x60), (0xc0, 0x90, 0xff)}
    found = False
    for yy in range(300, 500):
        for xx in range(264, 292):
            i = (yy * 1024 + xx) * 3
            if (pf[i], pf[i + 1], pf[i + 2]) in gtl_cols:
                found = True
                break
        if found:
            break
    check("gtl embedded icon drawn", found)
goto(100, 700); click(); time.sleep(0.2)       # 点外部关闭

# 双击 SYS 图标: 打开系统信息
vm.buf = ""
goto(32, 264); click(); click(); time.sleep(0.4)
check("system app opened", vm.expect("[gui] open system", 5000), vm.buf[-300:])
img_sys = vm.shot(os.path.join(SRC, "gui_sys.ppm"))
check("screendump system", img_sys is not None)
if img_sys:
    _, _, pz2 = img_sys
    i = (272 * 1024 + 600) * 3
    c = (pz2[i], pz2[i + 1], pz2[i + 2])
    print(f"       system title pixel {c}")
    check("system window drawn", c == (0x2a, 0x2e, 0x36))
goto(672, 276); click(); time.sleep(0.2)       # 关闭按钮

# 拖动 TERM 图标: 旧位置应恢复壁纸, 新位置出现图标
drag(32, 104, 90, 400)
img_idrag = vm.shot(os.path.join(SRC, "gui_icon_drag.ppm"))
check("screendump icon drag", img_idrag is not None)
if img_idrag:
    _, _, pd = img_idrag
    io = (96 * 1024 + 40) * 3
    inew = (392 * 1024 + 98) * 3
    old = (pd[io], pd[io + 1], pd[io + 2])
    new = (pd[inew], pd[inew + 1], pd[inew + 2])
    print(f"       icon drag old={old} new={new}")
    check("icon moved by drag", new == (0x2f, 0x6f, 0x9f) and
          old != (0x2f, 0x6f, 0x9f), f"old={old} new={new}")

# 拖入窗口: 把 FILES 图标拖到终端客户区 = 打开文件应用
vm.buf = ""
drag(32, 184, 600, 400)
check("drop into window", vm.expect("[gui] drop Files", 5000), vm.buf[-300:])
check("drop opens app", vm.expect("[gui] open files", 5000), vm.buf[-300:])
goto(100, 700); click(); time.sleep(0.2)       # 关闭文件窗口

# 再次点击已选中的 TERM 图标 = 打开终端
vm.buf = ""
goto(90, 400); click(); click(); time.sleep(0.3)
check("terminal app opened", vm.expect("[gui] open terminal", 5000),
      vm.buf[-300:])

# ---- 语言切换与 Linux 风格命令 ----
vm.buf = ""
vm.cmd("lang zh", 900)
check("LANG ZH", vm.expect("Language:", 4000), vm.buf[-200:])
vm.buf = ""
vm.cmd("lang", 800)
check("LANG ZH state", vm.expect("zh-CN", 4000), vm.buf[-200:])
vm.buf = ""
vm.cmd("lang en", 900)
check("LANG EN", vm.expect("Language: English", 4000), vm.buf[-200:])
vm.buf = ""
vm.cmd("uname -a", 900)
check("uname -a", vm.expect("x86_64", 4000), vm.buf[-200:])
vm.buf = ""
vm.cmd("neofetch", 1000)
check("neofetch", vm.expect("Kernel: GTK1", 4000), vm.buf[-200:])
vm.buf = ""
vm.cmd("free", 900)
check("free", vm.expect("Mem:", 4000), vm.buf[-200:])

vm.buf = ""
vm.cmd("textmode", 1200)
check("textmode command", vm.expect("[gui] text mode", 6000), vm.buf[-400:])
vm.buf = ""
vm.cmd("ver", 1000)
check("shell alive after GUI", vm.expect("GT-DOS", 6000), vm.buf[-300:])

print("RESULT:", "ALL PASS" if not FAIL else f"{len(FAIL)} FAILED -> {FAIL}")
vm.close()
sys.exit(0 if not FAIL else 1)
