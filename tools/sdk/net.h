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
 */
#ifndef _GT_NET_H
#define _GT_NET_H

struct net_info {
    unsigned ip, mask, gw, dns;
    unsigned char mac[6];
    unsigned char link;
    unsigned short mtu;
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

#endif /* _GT_NET_H */
