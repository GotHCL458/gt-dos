#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时验证: 图形文本模式(HELP 单页中文) + GUI 多窗口桌面 (抓屏像素分析).
   流程: 首启 OOBE(中文) -> 登录 -> HELP 完整性 -> gui ->
         鼠标点击图标开窗口 -> 拖拽 -> 关闭 -> 退出 -> 文本恢复."""

import codecs
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC_DISK = r"D:\GT-DOS\build\gt-dos-hd.img"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4800 + (os.getpid() % 60)
MPORT = SPORT + 1
FAIL = []


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAIL.append(name)


class VM:
    def __init__(self):
        args = [QEMU, "-m", "32", "-hda", self.disk, "-boot", "c",
                "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        if os.environ.get("QDBG"):
            args += ["-d", "int", "-D", os.path.join(TMP, "qemu_dbg.log")]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT)
        self.m = self._conn(MPORT)
        self.buf = ""

    disk = ""

    @staticmethod
    def _conn(port, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError:
                time.sleep(0.02)
        raise RuntimeError(f"port {port}")

    def feed(self, ms=500):
        self.s.settimeout(0.2)
        if not hasattr(self, "dec"):
            self.dec = codecs.getincrementaldecoder("utf-8")("replace")
        end = time.time() + ms / 1000.0
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d:
                    self.buf += self.dec.decode(d)
            except socket.timeout:
                pass
            except OSError:
                break

    def send(self, text):
        try:
            self.s.sendall(text.encode("utf-8", "replace"))
        except OSError:
            pass

    def cmd(self, line, wait_ms=700):
        self.send(line + "\r")
        self.feed(wait_ms)

    def expect(self, needle, timeout_ms=6000):
        end = time.time() + timeout_ms / 1000.0
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(250)
        return False

    def mon(self, line):
        try:
            self.m.sendall((line + "\n").encode())
        except OSError:
            pass
        time.sleep(0.08)
        self.m.settimeout(0.15)
        try:
            self.m.recv(4096)
        except Exception:
            pass

    def mmove(self, dx, dy):
        # PS/2 每包分量限 +-127
        while dx or dy:
            sx = max(-127, min(127, dx))
            sy = max(-127, min(127, dy))
            self.mon(f"mouse_move {sx} {sy}")
            dx -= sx
            dy -= sy

    def mclick(self, x, y):
        # 绝对定位: 从 (0,0) 出发不可知, 用 warp 到中心后相对移动.
        # 简化: 直接 mouse_move 到目标 (调用方保证当前在 0,0 参考系)
        self.mon("mouse_button 1")
        time.sleep(0.05)
        self.mon("mouse_button 0")

    def shot(self, name):
        path = os.path.join(TMP, name)
        if os.path.exists(path):
            os.remove(path)
        self.mon(f"screendump {path.replace(os.sep, '/')}")
        time.sleep(0.35)
        return path

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


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    tok = []
    pos = 0
    while len(tok) < 4:
        while pos < len(data) and data[pos] in (10, 32, 9, 13):
            pos += 1
        s = pos
        while pos < len(data) and data[pos] not in (10, 32, 9, 13):
            pos += 1
        tok.append(data[s:pos])
    pos += 1
    w, h = int(tok[1]), int(tok[2])
    px = data[pos:pos + w * h * 3]
    return w, h, px


def pget(px, w, x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]


def main():
    # 杀僵尸 QEMU
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                   capture_output=True)
    time.sleep(0.5)
    os.makedirs(TMP, exist_ok=True)
    VM.disk = os.path.join(TMP, "gui.img")
    shutil.copy(SRC_DISK, VM.disk)

    vm = VM()
    vm.feed(2000)
    check("boot -> OOBE", vm.expect("Language", 9000))
    vm.cmd("2")                       # 语言: 中文
    vm.cmd("GTPC")                    # 主机名
    vm.cmd("admin")                   # 管理员名
    vm.cmd("")                        # 口令留空 -> 跳过 (不设置)
    vm.feed(400)
    vm.cmd("1")                       # 主题: 经典
    check("oobe done", vm.expect("用户", 6000) or vm.expect("User:", 3000),
          vm.buf[-200:])

    # 登录 (admin, 无口令)
    vm.feed(800)
    vm.cmd("admin")
    vm.feed(500)
    vm.cmd("")
    check("shell prompt", vm.expect("admin@GTPC", 8000), vm.buf[-200:])

    # ---- HELP 单页完整中文 ----
    vm.cmd("help", 1500)
    ok_head = "命令一览" in vm.buf
    ok_tail = "PgUp/PgDn" in vm.buf
    check("HELP single page complete (head)", ok_head)
    check("HELP single page complete (tail)", ok_tail)
    check("HELP no '?' mojibake", "?" not in vm.buf.split("命令一览")[-1][:600])

    # 抓文本模式屏
    p = vm.shot("t_text.ppm")
    if os.path.exists(p):
        w, h, px = read_ppm(p)
        check("text mode is 720x400 gfx? no, VBE 640x480", (w, h) == (640, 480),
              f"{w}x{h}")
        # 背景应为调色板灰 (0xAAAAAA) 或主题色; 只要非全黑即可
        nonblack = sum(1 for i in range(0, len(px), 3 * 97)
                       if px[i] or px[i + 1] or px[i + 2])
        check("text screen has content", nonblack > 50, str(nonblack))
    else:
        check("screendump text", False)

    # ---- 高分辨率切换 ----
    vm.cmd("res 1024x768", 1000)
    check("res 1024x768 ack", "1024x768" in vm.buf, vm.buf[-120:])
    p = vm.shot("t_hi.ppm")
    if os.path.exists(p):
        w, h, px = read_ppm(p)
        check("high-res is 1024x768", (w, h) == (1024, 768), f"{w}x{h}")
        nonblack = sum(1 for i in range(0, len(px), 3 * 97)
                       if px[i] or px[i + 1] or px[i + 2])
        check("hi-res screen has content", nonblack > 50, str(nonblack))
    else:
        check("screendump hi-res", False)
    # 切回 640x480, 让后续 GUI 坐标测试有效
    vm.buf = ""
    vm.cmd("res 640x480", 1500)
    check("res 640 ack", "640x480" in vm.buf, vm.buf[-150:])
    p = vm.shot("t_back.ppm")
    if os.path.exists(p):
        w, h, px = read_ppm(p)
        check("res back to 640x480", (w, h) == (640, 480), f"{w}x{h}")

    # ---- 进入 GUI ----
    vm.cmd("gui", 1200)
    p = vm.shot("g_desktop.ppm")
    w, h, px = read_ppm(p)
    # 桌面渐变壁纸: 中央偏下像素应为蓝色系 (b > r)
    r, g, b = pget(px, w, 320, 300)
    check("desktop wallpaper gradient (blue)", b > r + 20, f"{r},{g},{b}")
    # 任务栏深蓝
    r, g, b = pget(px, w, 320, 470)
    check("taskbar drawn", b > r, f"{r},{g},{b}")
    # 菜单栏
    r, g, b = pget(px, w, 320, 10)
    check("menubar drawn", b > r, f"{r},{g},{b}")
    # 默认打开的 demo 窗口 (标题深蓝 0x0A246A) 在 (150,52) 附近
    r, g, b = pget(px, w, 150, 55)
    check("demo window titlebar", b > 60 and b > r, f"{r},{g},{b}")

    # 鼠标: 指针初始 warp 到 (320,240). 点击 about 图标中心 (596,144)
    vm.mmove(276, -96)
    vm.feed(300)
    p = vm.shot("g_move.ppm")
    w2, h2, px2 = read_ppm(p)
    # 指针黑色轮廓尖端 (596,144): 该处图标底色为 goldenrod(r=184), 黑尖 r<40
    r, g, b = pget(px2, w2, 596, 144)
    check("mouse pointer moved to icon", r < 40 and g < 40 and b < 40,
          f"{r},{g},{b}")
    # 向下移动 100: 指针尖端应到 (596,244) (修复"下移变上移"回归)
    vm.mmove(0, 100)
    vm.feed(300)
    p = vm.shot("g_move_down.ppm")
    w2, h2, px2 = read_ppm(p)
    r, g, b = pget(px2, w2, 596, 244)
    check("mouse pointer moved DOWN", r < 40 and g < 40 and b < 40,
          f"{r},{g},{b}")
    vm.mmove(0, -100)          # 回到图标位置
    vm.feed(200)
    # 点击 about 图标
    vm.mon("mouse_button 1"); time.sleep(0.05); vm.mon("mouse_button 0")
    vm.feed(400)
    p = vm.shot("g_about.ppm")
    w2, h2, px2 = read_ppm(p)
    # about 窗口打开在 (66,74)+ cascade -> 标题栏 (100,79) 深蓝
    r, g, b = pget(px2, w2, 100, 79)
    check("about window opened via icon", b > 60 and b > r, f"{r},{g},{b}")

    # 拖拽 about 标题栏: 指针 (40,144) -> 标题栏 (100,79), 再拖到 (250,179)
    vm.mmove(60, -65)
    vm.mon("mouse_button 1"); time.sleep(0.05)
    vm.mmove(150, 100)
    vm.mon("mouse_button 0")
    vm.feed(400)
    p = vm.shot("g_drag.ppm")
    w2, h2, px2 = read_ppm(p)
    # 拖拽后窗口主体应覆盖 (250,190) (经典灰 0xD4D0C8)
    r, g, b = pget(px2, w2, 250, 190)
    check("window dragged", abs(r - 212) < 12 and abs(g - 208) < 12 and abs(b - 200) < 12,
          f"{r},{g},{b}")

    # 关闭 about 窗口: 拖拽后窗口在 (216,109), 关闭按钮 (516,112); 指针 (250,179)
    vm.mmove(274, -60)
    vm.mon("mouse_button 1"); time.sleep(0.05); vm.mon("mouse_button 0")
    vm.feed(400)
    p = vm.shot("g_close.ppm")
    w2, h2, px2 = read_ppm(p)
    # 窗口关闭后 (400,200) 恢复桌面渐变 (蓝: b>r+20); 该点在 demo 窗口之外
    r, g, b = pget(px2, w2, 400, 200)
    check("about window closed", b > r + 20, f"{r},{g},{b}")

    # 退出: ESC
    vm.send("\x1b")
    vm.feed(1200)
    check("back to shell after ESC", vm.expect("admin@GTPC", 5000), vm.buf[-160:])
    p = vm.shot("g_exit.ppm")
    w2, h2, px2 = read_ppm(p)
    # 文本模式恢复: 屏幕上方应有文字像素 (非全黑)
    lit = sum(1 for i in range(0, min(len(px2), 640 * 200 * 3), 3)
              if px2[i] > 100)
    check("text console restored", lit > 100, str(lit))

    vm.close()
    print()
    if FAIL:
        print(f"FAILED {len(FAIL)}: {FAIL}")
        sys.exit(1)
    print("ALL GUI VERIFY PASSED")


if __name__ == "__main__":
    main()
