#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS 回归测试: QEMU 窗口 (PS/2) 输入可见性 + 滚动钳制.
   通过 monitor sendkey 注入键盘, screendump 抓屏, 像素分析验证."""

import os
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-i386.exe"
FLOPPY = r"D:\GT-DOS\build\gt-dos.img"
DISK = r"D:\GT-DOS\build\gt-dos-hdd.img"
TMP = r"D:\GT-DOS\build"
SPORT, MPORT = 4521, 4522


def connect(port, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), 0.5)
            return s
        except OSError:
            time.sleep(0.01)
    raise RuntimeError(f"cannot connect to port {port}")


def drain(sock, ms):
    sock.settimeout(0.2)
    out = b""
    end = time.time() + ms / 1000.0
    while time.time() < end:
        try:
            d = sock.recv(4096)
            if d:
                out += d
        except socket.timeout:
            pass
    return out.decode("utf-8", "replace")


def mon(m, cmd):
    m.sendall((cmd + "\n").encode())
    time.sleep(0.12)
    try:
        m.recv(4096)
    except Exception:
        pass


def shot(m, name):
    p = os.path.join(TMP, name)
    if os.path.exists(p):
        os.remove(p)
    mon(m, f"screendump {p.replace(os.sep, '/')}")
    time.sleep(0.25)
    return p


def cells_ink(path, row):
    """返回该行每个字符格的墨量列表 (80 个)."""
    with open(path, "rb") as f:
        data = f.read()
    i = 0; toks = []
    while len(toks) < 4:
        while i < len(data) and data[i] in (10, 32, 9): i += 1
        s = i
        while i < len(data) and data[i] not in (10, 32, 9): i += 1
        toks.append(data[s:i]); i += 1
    w, h = int(toks[1]), int(toks[2]); px = i
    y0, y1 = row * 16, min(row * 16 + 16, h)
    prof = []
    for c in range(80):
        x0, x1 = c * 9, c * 9 + 9
        ink = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                if x >= w or y >= h: continue
                k = px + (y * w + x) * 3
                if data[k] > 60 or data[k + 1] > 60 or data[k + 2] > 60:
                    ink += 1
        prof.append(ink)
    return prof


def count_yellow(path):
    """统计右下角区域黄色像素数 (回滚指示条)."""
    data = open(path, "rb").read()
    i = 0; toks = []
    while len(toks) < 4:
        while i < len(data) and data[i] in (10, 32, 9): i += 1
        s = i
        while i < len(data) and data[i] not in (10, 32, 9): i += 1
        toks.append(data[s:i]); i += 1
    w, h = int(toks[1]), int(toks[2]); px = i
    n = 0
    for y in range(int(h * 0.88), h, 2):
        for x in range(int(w * 0.4), w, 2):
            k = px + (y * w + x) * 3
            if data[k] > 200 and data[k + 1] > 200 and data[k + 2] < 120:
                n += 1
    return n


def main():
    args = [QEMU, "-m", "32", "-fda", FLOPPY, "-hda", DISK, "-boot", "a",
            "-display", "none",
            "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
            "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        s = connect(SPORT)
        m = connect(MPORT)
        drain(s, 2500)

        # 1) 提示符 "C:\>" 占第 0-3 格, 光标第 4 格
        p1 = shot(m, "t_prompt.ppm")
        c1 = cells_ink(p1, 24)
        prompt_has_text = sum(c1[0:4]) > 20

        # 2) PS/2 输入 "dir" 不回车 -> 第 4,5,6 格应出现文字 (编辑行可见)
        for ch in "dir":
            mon(m, f"sendkey {ch}")
        time.sleep(0.4)
        p2 = shot(m, "t_typed.ppm")
        c2 = cells_ink(p2, 24)
        # "dir" 落在第 4,5,6 格; 只要这三格里有足够墨即证明输入可见
        typed_ink = c2[4] + c2[5] + c2[6]
        ok_edit = typed_ink > 30 and prompt_has_text

        # 3) 回车执行 -> 输出可见, 新提示符行
        mon(m, "sendkey ret")
        drain(s, 2000)
        p3 = shot(m, "t_after.ppm")
        c3 = cells_ink(p3, 24)

        # 4) 滚动: Ctrl+Up x3 -> 指示条出现; 回到底 -> 消失
        for _ in range(3):
            mon(m, "sendkey ctrl-up")
        time.sleep(0.3)
        p4 = shot(m, "t_scroll.ppm")
        yellow = count_yellow(p4)
        ok_scroll_ind = yellow > 20

        for _ in range(12):
            mon(m, "sendkey ctrl-down")
        time.sleep(0.6)
        p5 = shot(m, "t_bottom.ppm")
        yellow5 = count_yellow(p5)
        ok_bottom = yellow5 == 0

        s.sendall(b"halt\r")
        drain(s, 800)
        s.close()
        m.close()
    finally:
        if proc.poll() is None:
            proc.kill()

    print(f"prompt cells[0:4] ink   = {c1[0:4]}")
    print(f"after 'dir' cells[4:7]  = {c2[4:7]}  (sum={typed_ink}, expect >30)")
    print(f"scroll yellow           = {yellow}")
    print(f"bottom yellow           = {yellow5}")
    print()
    print("EDITING-LINE VISIBLE :", "PASS" if ok_edit else "FAIL")
    print("SCROLL INDICATOR     :", "PASS" if ok_scroll_ind else "FAIL")
    print("BOTTOM CLAMP CLEAN   :", "PASS" if ok_bottom else "FAIL")
    for p in (p1, p2, p3, p4, p5):
        try:
            os.remove(p)
        except OSError:
            pass
    return 0 if (ok_edit and ok_scroll_ind and ok_bottom) else 1


if __name__ == "__main__":
    sys.exit(main())
