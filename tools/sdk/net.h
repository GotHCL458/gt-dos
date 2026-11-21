/* GT-DOS SDK: TCP/IP socket builtins (INT 0x21 wrappers).
 *
 * Addresses use the IP4() form, e.g. ip4(10,0,2,2).
 * A user program has no heap; use a scratch region above its image for
 * receive buffers, for example 0x700000 (below the 0x7FF000 stack).
 *
 *   int fd = tcp_connect(ip4(10,0,2,2), 7777);
 *   tcp_send(fd, "hello\n", 6);
 *   int buf = 0x700000;
 *   int n = tcp_recv(fd, buf, 64);
 *   if (n > 0) write(1, buf, n);
 *   tcp_close(fd);
 *
 * UDP has no connection state: send/receive one datagram at a time.
 *   int u = udp_open(0);                     // 0 = any local port
 *   udp_sendto(u, ip4(10,0,2,2), 7777, "hi", 2);
 *   int n = udp_recvfrom(u, buf, 512, 2000, 0, 0);  // 2000 ms timeout
 *   udp_close(u);
 *   // n = -2 means an ICMP error (e.g. port unreachable) was received.
 */
#ifndef _GT_NET_H
#define _GT_NET_H

struct net_info {
    unsigned ip, mask, gw, dns;
    unsigned char mac[6];
    unsigned char link;
    unsigned short mtu;
};

struct icmp_err {
    unsigned src;                    /* ICMP 报文来源 */
    unsigned dst;                    /* 出错报文原目的地 */
    unsigned short sport, dport;     /* 内层端口 */
    unsigned char type, code;        /* 3=不可达, 11=超时 */
    unsigned char proto;             /* 17=UDP, 6=TCP */
    unsigned short reserved;
    unsigned info;
};

int  ip4(int a, int b, int c, int d);     /* compile-time helper */
int  netinfo(struct net_info *out);       /* SYS_NETINFO (21) */
int  tcp_connect(int ip, int port);       /* SYS_TCP_CONNECT (22) */
int  tcp_send(int fd, void *buf, int n);  /* SYS_TCP_SEND (23) */
int  tcp_recv(int fd, void *buf, int n);  /* SYS_TCP_RECV (24) */
int  tcp_close(int fd);                   /* SYS_TCP_CLOSE (25) */
int  tcp_listen(int port);                /* SYS_TCP_LISTEN (26) */
int  tcp_accept(int fd);                  /* SYS_TCP_ACCEPT (27) */
int  dhcp(void);                          /* SYS_DHCP (28) */
int  dns(char *name);                     /* SYS_DNS (29), 返回 IPv4 或 0 */
int  tftp_get(int ip, char *remote, char *local); /* SYS_TFTP_GET (30) */
int  http_get(char *hostport, char *path, char *local); /* SYS_HTTP_GET (31) */

/* UDP 用户 API (SYS_UDP_*) */
int  udp_open(int port);                  /* 32: 0 = 自动分配, 返回 fd */
int  udp_sendto(int fd, int ip, int port, void *data, int len);   /* 33 */
int  udp_recvfrom(int fd, void *buf, int cap, int timeout_ms,
                  int *src_ip, int *src_port);                    /* 34 */
int  icmp_err(struct icmp_err *out);      /* 35: 1=取到新差错 */
int  udp_close(int fd);                   /* 36 */

/* net.c 标准库辅助 (包含本头文件后按需编入) */
int  net_aton(char *s);                       /* 点分十进制 -> IPv4 */
int  net_resolve(char *host);                 /* aton/dns 解析 */
int  net_connect(char *host, int port);       /* 解析 + tcp_connect */
int  net_send_str(int fd, char *s);           /* tcp_send + strlen */
int  net_send_all(int fd, char *buf, int len);/* 循环发送直到完成 */
int  net_recv_line(int fd, char *buf, int cap); /* 读一行, 含换行并补 NUL */
int  net_recv_all(int fd, char *buf, int cap);  /* 读到关闭/缓冲满 */

#endif /* _GT_NET_H */
