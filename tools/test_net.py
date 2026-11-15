#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GT-DOS 网络栈验证: RTL8139 + ARP/IPv4(分片重组)/ICMP(含差错)/UDP/TCP/DNS/TFTP/HTTP.
   宿主机启动 TCP 回显、UDP 回显、TFTP 与 HTTP 服务, 来宾通过 QEMU 用户网络
   (10.0.2.2 = 宿主机) 访问, 验证 shell、TC 用户程序与文件下载."""
import os, shutil, socket, struct, subprocess, sys, threading, time

QEMU = r"D:\GT-DOS\qemu\qemu-system-x86_64.exe"
SRC = r"D:\GT-DOS\build"
TMP = r"D:\GT-DOS\build\testrun"
SPORT = 4800 + (os.getpid() % 80)
MPORT = SPORT + 1
TPORT = 20000 + (os.getpid() % 2000)
TFTP_PORT = 21000 + (os.getpid() % 1000)
HTTP_PORT = 22000 + (os.getpid() % 1000)
UDP_PORT = 23000 + (os.getpid() % 1000)
CLOSED_PORT = UDP_PORT + 1
TFTP_BODY = b"TFTP-FILE-START\n" + b"A" * 1100 + b"TFTP-FILE-END\n"
HTTP_BODY = b"HTTP-FILE-START\n" + b"B" * 3000 + b"HTTP-FILE-END\n"
FAILURES = []

def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)

def echo_server(srv):
    while True:
        try:
            c, _ = srv.accept()
        except OSError:
            return
        try:
            data = c.recv(1024)
            if b"hello" in data:
                c.sendall(b"NET OK\n")
            else:
                c.sendall(b"ECHO: " + data + b"\n")
        except OSError:
            pass
        finally:
            c.close()

def tftp_server(sock):
    while True:
        try:
            data, addr = sock.recvfrom(1024)
        except OSError:
            return
        if len(data) < 4 or struct.unpack("!H", data[:2])[0] != 1:
            continue
        block, off = 1, 0
        while True:
            chunk = TFTP_BODY[off:off + 512]
            pkt = struct.pack("!HH", 3, block) + chunk
            for _ in range(5):
                sock.sendto(pkt, addr)
                sock.settimeout(1.0)
                try:
                    ack, _ = sock.recvfrom(64)
                except socket.timeout:
                    continue
                if len(ack) >= 4 and struct.unpack("!HH", ack[:4]) == (4, block):
                    break
            off += len(chunk)
            block = (block + 1) & 0xFFFF
            if len(chunk) < 512:
                break

def udp_server(sock):
    while True:
        try:
            data, addr = sock.recvfrom(8192)
        except OSError:
            return
        try:
            sock.sendto(data, addr)
        except OSError:
            pass

def http_server(sock):
    while True:
        try:
            c, _ = sock.accept()
        except OSError:
            return
        try:
            c.recv(2048)
            hdr = (b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                   b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(HTTP_BODY))
            c.sendall(hdr + HTTP_BODY)
        except OSError:
            pass
        finally:
            c.close()

class VM:
    def __init__(self):
        args = [QEMU, "-m", "32", "-hda", os.path.join(TMP, "hd.img"),
                "-boot", "c", "-display", "none",
                "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
                "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
                "-netdev", "user,id=n0",
                "-device", "rtl8139,netdev=n0",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.s = self._conn(SPORT); self.m = self._conn(MPORT); self.buf = ""
    @staticmethod
    def _conn(port, t=8.0):
        end = time.time() + t
        while time.time() < end:
            try: return socket.create_connection(("127.0.0.1", port), 0.5)
            except OSError: time.sleep(0.02)
        raise RuntimeError(port)
    def feed(self, ms=500):
        self.s.settimeout(0.2); end = time.time() + ms/1000
        while time.time() < end:
            try:
                d = self.s.recv(8192)
                if d: self.buf += d.decode("utf-8", "replace")
            except socket.timeout: pass
            except OSError: break
    def cmd(self, line, wait_ms=700):
        try: self.s.sendall((line + "\r").encode())
        except OSError: pass
        self.feed(wait_ms)
    def send(self, raw):
        try: self.s.sendall(raw.encode())
        except OSError: pass
    def expect(self, needle, timeout_ms=6000):
        end = time.time() + timeout_ms/1000; low = needle.lower()
        while time.time() < end:
            if low in self.buf.lower(): return True
            self.feed(300)
        return False
    def close(self):
        for x in (self.s, self.m):
            try: x.close()
            except Exception: pass
        if self.p.poll() is None:
            self.p.kill()
            try: self.p.wait(5)
            except Exception: pass

def main():
    os.makedirs(TMP, exist_ok=True)
    shutil.copy(os.path.join(SRC, "gt-dos-hd.img"), os.path.join(TMP, "hd.img"))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", TPORT))
    srv.listen(8)
    threading.Thread(target=echo_server, args=(srv,), daemon=True).start()

    tsrv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tsrv.bind(("127.0.0.1", TFTP_PORT))
    threading.Thread(target=tftp_server, args=(tsrv,), daemon=True).start()

    hsrv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hsrv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    hsrv.bind(("127.0.0.1", HTTP_PORT))
    hsrv.listen(8)
    threading.Thread(target=http_server, args=(hsrv,), daemon=True).start()

    usrv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    usrv.bind(("127.0.0.1", UDP_PORT))
    threading.Thread(target=udp_server, args=(usrv,), daemon=True).start()

    vm = VM(); vm.feed(3000)
    vm.expect("Language", 6000)
    vm.cmd("1"); vm.cmd("SBOX"); vm.cmd("admin"); vm.cmd("")
    check("oobe done", vm.expect("Enjoy GT-DOS", 6000), vm.buf[-300:])
    vm.send("\x1b"); vm.feed(500)
    check("login screen", vm.expect("User:", 8000))
    vm.cmd("admin"); vm.expect("Password", 3000); vm.cmd("")
    check("at prompt", vm.expect(">", 4000))

    vm.buf = ""
    vm.cmd("net", 1200)
    check("net shows RTL8139", vm.expect("RTL8139", 4000), vm.buf[-400:])
    check("net shows static IP", vm.expect("10.0.2.15", 3000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd("ping 10.0.2.2 2", 2500)
    check("icmp echo reply", vm.expect("reply from 10.0.2.2", 5000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd("dhcp", 2500)
    check("dhcp lease", vm.expect("DHCP: 10.0.2.15", 5000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"tcp 10.0.2.2 {TPORT} NET-TEST", 3000)
    check("tcp shell client", vm.expect(f"ECHO: NET-TEST", 5000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"tcp 10.0.2.2 {CLOSED_PORT} HI", 2500)
    check("tcp connect refused (RST)", vm.expect("TCP connect failed", 6000),
          vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"udp 10.0.2.2 {UDP_PORT}", 2500)
    check("udp echo", vm.expect("UDP: sent 24, received 24", 6000), vm.buf[-400:])
    check("udp payload match", vm.expect("(match)", 3000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"udp 10.0.2.2 {UDP_PORT} 3000", 4000)
    check("udp large datagram (IP fragments)",
          vm.expect("UDP: sent 3000, received 3000", 10000), vm.buf[-400:])
    check("udp reassembled payload", vm.expect("(match)", 3000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"udp 10.0.2.2 {CLOSED_PORT}", 3000)
    check("udp closed port times out (no crash)",
          vm.expect("UDP: no reply", 8000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd("put n.c int main(void){")
    vm.cmd(f"append n.c int fd=tcp_connect(ip4(10,0,2,2),{TPORT});")
    vm.cmd('append n.c if(fd<0){print("CONNECT FAIL\\n");return 1;}')
    vm.cmd('append n.c print("CONNECTED\\n");')
    vm.cmd('append n.c tcp_send(fd,"hello\\n",6);')
    vm.cmd("append n.c int b=0x700000;")
    vm.cmd("append n.c int n=tcp_recv(fd,b,64);")
    vm.cmd("append n.c if(n>0)write(1,b,n);")
    vm.cmd("append n.c tcp_close(fd);")
    vm.cmd("append n.c return 0;")
    vm.cmd("append n.c }")
    vm.cmd("tc n.c", 3000)
    check("tc compiles net program", vm.expect("Compiled ", 20000), vm.buf[-400:])
    vm.cmd("run n.gtp", 4000)
    check("tc net program connected", vm.expect("CONNECTED", 12000), vm.buf[-400:])
    check("tc net program received", vm.expect("NET OK", 12000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd("put u.c int main(void){")
    vm.cmd("append u.c int u=udp_open(0);")
    vm.cmd("append u.c int b=0x700000;")
    vm.cmd(f'append u.c udp_sendto(u,ip4(10,0,2,2),{UDP_PORT},"tcu",3);')
    vm.cmd("append u.c udp_recvfrom(u,b,64,2000,0,0);")
    vm.cmd("append u.c write(1,b,3);")
    vm.cmd("append u.c udp_close(u);")
    vm.cmd("append u.c return 0;")
    vm.cmd("append u.c }")
    vm.cmd("tc u.c", 3000)
    check("tc compiles udp program", vm.expect("Compiled ", 20000), vm.buf[-400:])
    vm.cmd("run u.gtp", 4000)
    check("tc udp six-arg syscalls", vm.expect("tcu", 12000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"tftp get 10.0.2.2:{TFTP_PORT} remote.txt got.txt", 5000)
    check("tftp download", vm.expect(f"TFTP: {len(TFTP_BODY)} bytes", 15000), vm.buf[-400:])
    vm.cmd("cat got.txt", 2500)
    check("tftp file content", vm.expect("TFTP-FILE-END", 10000), vm.buf[-400:])

    vm.buf = ""
    vm.cmd(f"http get 10.0.2.2:{HTTP_PORT} /x.txt got2.txt", 5000)
    check("http download", vm.expect(f"HTTP: {len(HTTP_BODY)} bytes", 15000), vm.buf[-400:])
    vm.cmd("cat got2.txt", 2500)
    check("http file content", vm.expect("HTTP-FILE-END", 12000), vm.buf[-400:])

    vm.cmd("shutdown"); vm.feed(800); vm.close()
    for s in (srv, tsrv, hsrv, usrv):
        try: s.close()
        except Exception: pass

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {FAILURES}"); return 1
    print("RESULT: ALL PASS"); return 0

if __name__ == "__main__":
    sys.exit(main())
