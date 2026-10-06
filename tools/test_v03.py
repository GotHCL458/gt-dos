#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""GT-DOS v0.7 全流程测试:
   1) 首启 -> OOBE 向导 (语言/主机名/admin/主题)
   2) 重启 -> OOBE 不再出现 (持久化), 进入登录界面
   3) 以 admin 登录 -> whoami/users/config/df
   4) 普通用户权限测试 (useradd 被拒)
   5) 数据持久化: 建文件+用户 -> 重启 -> 仍在
   6) 系统保护: \GT-DOS\ 不可删, 头文件受保护
   7) SHUTDOWN -> QEMU 经 isa-debug-exit 退出, 校验退出码
   单硬盘镜像 (MBR 引导), 用临时副本不污染 build 母本."""

import os
import shutil
import socket
import subprocess
import sys
import time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC_DISK = r"D:\GT-DOS\build\gt-dos-hd.img"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4700 + (os.getpid() % 80)
MPORT = SPORT + 1

FAILURES = []


def check(name, cond, detail="", dump=None):
    tag = "PASS" if cond else "FAIL"
    print(f"[{tag}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond and dump:
        print("      ---- serial tail ----")
        for ln in dump[-600:].splitlines():
            print("      | " + ln)
        print("      -----------------------")
    if not cond:
        FAILURES.append(name)


class VM:
    def __init__(self, disk, sport, mport):
        args = [QEMU, "-m", "32", "-hda", disk, "-boot", "c",
                "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{mport},server,nowait",
                "-serial", f"tcp:127.0.0.1:{sport},server,nowait",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        self.s = self._connect(sport)
        self.m = self._connect(mport)
        self.buf = ""

    @staticmethod
    def _connect(port, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError:
                time.sleep(0.02)
        raise RuntimeError(f"port {port}")

    def feed(self, ms=500):
        self.s.settimeout(0.2)
        end = time.time() + ms / 1000.0
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d:
                    self.buf += d.decode("utf-8", "replace")
            except socket.timeout:
                pass
            except OSError:           # QEMU 退出 -> 连接重置
                break

    def send(self, text):
        self.s.sendall(text.encode("utf-8", "replace"))

    def cmd(self, line, wait_ms=800):
        try:
            self.send(line + "\r")
        except OSError:
            pass
        self.feed(wait_ms)

    def expect(self, needle, timeout_ms=6000):
        end = time.time() + timeout_ms / 1000.0
        low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower():
                return True
            self.feed(300)
        return False

    def tail(self, n=400):
        return self.buf[-n:]

    def alive(self):
        return self.p.poll() is None

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


def boot_vm(disk, tag):
    vm = VM(disk, SPORT, MPORT)
    vm.feed(1500)
    return vm


def main():
    os.makedirs(TMP, exist_ok=True)
    disk = os.path.join(TMP, "hd.img")
    shutil.copy(SRC_DISK, disk)

    # ================= 阶段 1: 首启 OOBE =================
    print("--- phase 1: first boot -> OOBE ---")
    vm = boot_vm(disk, "oobe")
    check("boot banner", vm.expect("GT-DOS", 8000))
    check("OOBE starts automatically", vm.expect("Language", 8000),
          dump=vm.buf)
    check("OOBE step 1 language", vm.expect("Chinese", 3000))

    vm.cmd("1")                          # 语言: English
    vm.cmd("GTBOX")                      # 主机名
    check("hostname accepted", vm.expect("GTBOX", 2000))
    vm.cmd("admin")                      # 管理员用户名
    check("admin user accepted", vm.expect("admin", 2000))
    vm.cmd("s3cret")                     # 密码
    vm.cmd("s3cret")                     # 确认
    check("password set", vm.expect("Password set for 'admin'", 3000))
    vm.cmd("2")                          # 主题: 绿底黑
    check("theme applied", vm.expect("Theme applied", 3000))
    check("OOBE complete", vm.expect("Setup complete", 3000))

    # ================= 阶段 2: 登录 =================
    print("--- phase 2: login ---")
    check("login screen", vm.expect("GT-DOS Login", 4000))
    vm.cmd("admin")
    vm.cmd("wrongpass")
    check("wrong password rejected", vm.expect("Wrong password", 3000))
    vm.cmd("admin")
    vm.cmd("s3cret")
    check("login ok", vm.expect("Welcome, admin", 4000))
    check("prompt shows user@host", vm.expect("admin@GTBOX C:\\>", 3000))

    # ================= 阶段 3: 用户/配置命令 =================
    print("--- phase 3: user & config commands ---")
    vm.cmd("whoami")
    check("whoami = admin", vm.expect("admin (admin)", 3000))
    vm.cmd("users")
    check("users lists admin", vm.expect("admin", 3000))
    vm.cmd("config show")
    check("config shows GTBOX", vm.expect("GTBOX", 3000))
    check("config shows oobe done", vm.expect("oobe_done  = yes", 3000))
    vm.cmd("df")
    check("df shows GTOS.CFG", vm.expect("GTOS.CFG", 3000))
    check("df shows USERS.SYS", vm.expect("USERS.SYS", 3000))

    # 建普通用户 + 文件
    vm.cmd("useradd bob")
    check("useradd prompt", vm.expect("Password (empty = none)", 3000))
    vm.cmd("bobpw")
    vm.cmd("bobpw")
    check("bob created", vm.expect("User 'bob' created", 3000))
    vm.cmd("put note.txt persisted-data-42")
    check("file written", vm.expect("Wrote ", 3000))

    # ================= 阶段 4: 重启 -> 持久化 =================
    print("--- phase 4: reboot -> persistence ---")
    vm.close()
    time.sleep(0.5)
    vm = boot_vm(disk, "reboot")
    check("no OOBE after reboot", not vm.expect("Language", 2500),
          "OOBE should be skipped")
    vm.send("\x1b")
    vm.feed(500)
    check("login screen after reboot", vm.expect("User:", 8000),
          dump=vm.buf)
    vm.cmd("admin")
    vm.cmd("s3cret")
    check("login with old password", vm.expect("Welcome, admin", 4000))
    vm.cmd("cat note.txt")
    check("file persisted", vm.expect("persisted-data-42", 4000))
    vm.cmd("users")
    check("bob persisted", vm.expect("bob", 4000))

    # ================= 阶段 5: 权限控制 =================
    print("--- phase 5: permission model ---")
    vm.cmd("logout")
    check("re-login screen", vm.expect("GT-DOS Login", 4000))
    vm.cmd("bob")
    vm.cmd("bobpw")
    check("bob logged in", vm.expect("Welcome, bob", 4000))
    check("bob prompt", vm.expect("bob@GTBOX C:\\>", 3000))
    vm.cmd("useradd eve")
    check("non-admin useradd denied",
          vm.expect("Permission denied", 3000))
    vm.cmd("shutdown")
    check("non-admin shutdown denied",
          vm.expect("Permission denied", 3000))

    # ================= 阶段 6: 系统保护 =================
    print("--- phase 6: system protection ---")
    vm.cmd("del \\GT-DOS\\GTOS.CFG")
    check("delete GTOS.CFG blocked", vm.expect("Cannot delete", 3000),
          dump=vm.buf)
    vm.cmd("rmdir \\GT-DOS")
    check("rmdir GT-DOS blocked", vm.expect("Cannot remove", 3000))
    vm.cmd("del \\GT-DOS\\INCLUDE\\STDIO.H")
    check("delete header blocked", vm.expect("Cannot delete", 3000))

    # ================= 阶段 7: 关机 (admin) =================
    print("--- phase 7: shutdown ---")
    vm.cmd("logout")
    vm.expect("GT-DOS Login", 4000)
    vm.cmd("admin")
    vm.cmd("s3cret")
    vm.expect("admin@GTBOX", 4000)
    vm.cmd("shutdown")
    code = None
    try:
        code = vm.p.wait(10)
    except Exception:
        pass
    check("QEMU exited on shutdown", code is not None,
          f"exit={code}")
    if code is not None:
        check("shutdown exit code", code in (0, 0x21), f"got {hex(code)}")
    vm.close()

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}")
        return 1
    print("RESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
