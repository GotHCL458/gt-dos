/* 网络应用层: DNS 解析、TFTP 下载、HTTP/1.0 GET 下载.
   文件内容先收进 64KB 内存缓冲, 完成后写入 exFAT (受当前用户权限约束). */
#include "gt.h"

static u8 dl_buf[65536] __attribute__((section(".highbss")));

/* --------------------------------------------------------------- DNS */
static int dns_skip_name(const u8 *p, int n, int off)
{
    while (off < n) {
        u8 l = p[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)
            return off + 2;
        if (l > 63)
            return -1;
        off += 1 + (int)l;
    }
    return -1;
}

int net_dns_resolve(const char *name, u32 *out)
{
    if (!name || !name[0] || !out || !net_init())
        return -1;
    int fd = udp_open(0);
    if (fd < 0)
        return -1;

    u8 q[512];
    memset(q, 0, sizeof(q));
    u16 id = (u16)(timer_ticks() & 0xFFFF);
    net_wr_be16(q + 0, id);
    net_wr_be16(q + 2, 0x0100);         /* RD */
    net_wr_be16(q + 4, 1);              /* QDCOUNT */
    u32 o = 12;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.')
            dot++;
        u32 l = (u32)(dot - p);
        if (l == 0 || l > 63 || o + l + 3 >= sizeof(q)) {
            udp_close(fd);
            return -1;
        }
        q[o++] = (u8)l;
        memcpy(q + o, p, l);
        o += l;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    q[o++] = 0;
    net_wr_be16(q + o, 1); o += 2;      /* QTYPE A */
    net_wr_be16(q + o, 1); o += 2;      /* QCLASS IN */

    if (udp_sendto(fd, net_dns(), 53, q, o) < 0) {
        udp_close(fd);
        return -1;
    }
    u8 r[512];
    int n = udp_recvfrom(fd, r, sizeof(r), 2000, 0, 0);
    udp_close(fd);
    if (n < 12 || net_be16(r) != id || !(r[2] & 0x80))
        return -1;
    u32 an = net_be16(r + 6);
    if (an == 0)
        return -1;

    int off = dns_skip_name(r, n, 12);
    if (off < 0 || off + 4 > n)
        return -1;
    off += 4;
    for (u32 i = 0; i < an; i++) {
        off = dns_skip_name(r, n, off);
        if (off < 0 || off + 10 > n)
            return -1;
        u16 type = net_be16(r + off);
        u16 cls = net_be16(r + off + 2);
        u16 rdlen = net_be16(r + off + 8);
        off += 10;
        if (off + rdlen > n)
            return -1;
        if (type == 1 && cls == 1 && rdlen == 4) {
            *out = net_be32(r + off);
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

/* -------------------------------------------------------------- TFTP */
static void tftp_ack(int fd, u32 ip, u16 port, u16 block)
{
    u8 a[4];
    net_wr_be16(a, 4);
    net_wr_be16(a + 2, block);
    udp_sendto(fd, ip, port, a, 4);
}

int net_tftp_fetch(u32 server, u16 port, const char *remote, const char *local)
{
    if (!server || !remote || !remote[0] || !local || !local[0] || !net_init())
        return -1;
    size_t rl = strlen(remote);
    if (rl > 500)
        return -1;
    int fd = udp_open(0);
    if (fd < 0)
        return -1;

    u8 rrq[600];
    u32 o = 0;
    net_wr_be16(rrq, 1);
    o = 2;
    memcpy(rrq + o, remote, rl + 1);
    o += (u32)rl + 1;
    memcpy(rrq + o, "octet", 6);
    o += 6;

    u32 srv_ip = server;
    u16 srv_port = port;
    if (udp_sendto(fd, server, port, rrq, o) < 0) {
        udp_close(fd);
        return -1;
    }

    u16 block = 1;
    u32 total = 0;
    int retry = 0;
    u8 rbuf[600];
    bool done = false;

    while (!done) {
        int n = udp_recvfrom(fd, rbuf, sizeof(rbuf), 1500, &srv_ip, &srv_port);
        if (n < 0) {
            if (++retry > 5)
                break;
            if (block == 1)
                udp_sendto(fd, server, port, rrq, o);
            else
                tftp_ack(fd, srv_ip, srv_port, (u16)(block - 1));
            continue;
        }
        u16 op = net_be16(rbuf);
        if (op == 5)                    /* ERROR */
            break;
        if (op != 3 || n < 4)
            continue;
        u16 b = net_be16(rbuf + 2);
        u32 dlen = (u32)n - 4;
        if (b == block) {
            if (total + dlen > sizeof(dl_buf))
                break;
            if (dlen)
                memcpy(dl_buf + total, rbuf + 4, dlen);
            total += dlen;
            tftp_ack(fd, srv_ip, srv_port, block);
            retry = 0;
            if (dlen < 512)
                done = true;
            else
                block++;
        } else if (b + 1 == block) {
            tftp_ack(fd, srv_ip, srv_port, b);   /* 重复块 */
        }
    }
    udp_close(fd);
    if (!done)
        return -1;
    if (!fat_create(local, dl_buf, total))
        return -1;
    return (int)total;
}

/* -------------------------------------------------------------- HTTP */
static void split_hostport(const char *hp, char *host, int cap, u16 *port)
{
    int k = 0;
    const char *p = hp;
    while (*p && *p != ':' && k < cap - 1)
        host[k++] = *p++;
    host[k] = 0;
    *port = 80;
    if (*p == ':')
        *port = (u16)atoi_dec(p + 1);
}

static int ci_find(const u8 *hay, u32 n, const char *needle)
{
    u32 nl = (u32)strlen(needle);
    if (nl == 0 || n < nl)
        return -1;
    for (u32 i = 0; i + nl <= n; i++) {
        u32 j = 0;
        while (j < nl && tolower_((u8)hay[i + j]) == tolower_((u8)needle[j]))
            j++;
        if (j == nl)
            return (int)i;
    }
    return -1;
}

int net_http_fetch(const char *hostport, const char *path, const char *local)
{
    if (!hostport || !path || !local || !local[0] || !net_init())
        return -1;
    char host[128];
    u16 port;
    split_hostport(hostport, host, sizeof(host), &port);
    if (!host[0] || port == 0)
        return -1;

    u32 ip;
    if (!net_parse_ip(host, &ip)) {
        if (net_dns_resolve(host, &ip) < 0)
            return -1;
    }

    int fd = tcp_connect(ip, port);
    if (fd < 0)
        return -1;

    char req[512];
    int k = 0;
    const char *a = "GET ";
    while (*a && k < 480) req[k++] = *a++;
    if (k < 480) req[k++] = '/';
    const char *q = path[0] == '/' ? path + 1 : path;
    while (*q && k < 480) req[k++] = *q++;
    a = " HTTP/1.0\r\nHost: ";
    while (*a && k < 480) req[k++] = *a++;
    for (const char *h = host; *h && k < 480; h++) req[k++] = *h;
    a = "\r\nConnection: close\r\n\r\n";
    while (*a && k < 480) req[k++] = *a++;
    req[k] = 0;

    if (tcp_send(fd, req, (u32)k) < 0) {
        tcp_close(fd);
        return -1;
    }

    u32 total = 0;
    u32 last = timer_uptime_ms();
    while (total < sizeof(dl_buf)) {
        int n = tcp_recv(fd, dl_buf + total, (u32)sizeof(dl_buf) - total);
        if (n > 0) {
            total += (u32)n;
            last = timer_uptime_ms();
            continue;
        }
        if (timer_uptime_ms() - last > 1500)
            break;
        break;
    }
    tcp_close(fd);

    u32 body = 0;
    for (u32 i = 0; i + 4 <= total; i++) {
        if (dl_buf[i] == '\r' && dl_buf[i + 1] == '\n' &&
            dl_buf[i + 2] == '\r' && dl_buf[i + 3] == '\n') {
            body = i + 4;
            break;
        }
    }
    if (body == 0)
        return -1;

    u32 blen = total - body;
    int cl = ci_find(dl_buf, body, "Content-Length:");
    if (cl >= 0) {
        u32 v = 0;
        u32 i = (u32)cl + 15;
        while (i < body && (dl_buf[i] == ' ' || dl_buf[i] == '\t'))
            i++;
        while (i < body && dl_buf[i] >= '0' && dl_buf[i] <= '9')
            v = v * 10 + (u32)(dl_buf[i++] - '0');
        if (v && v < blen)
            blen = v;
    }
    if (!fat_create(local, dl_buf + body, blen))
        return -1;
    return (int)blen;
}
