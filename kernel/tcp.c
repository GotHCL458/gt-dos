/* TCP (主动连接/被动监听, 数据收发, 重传, 四次挥手).
   单连接阻塞式 socket 表, 由 INT 0x21 与 shell 调用.
   收包经 net_poll() -> tcp_input(); 重传与等待都用轮询. */
#include "gt.h"

#define TCP_MAX    8
#define TCP_RXBUF  4096
#define TCP_MSS    1460
#define TCP_WIN    TCP_RXBUF

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum {
    TS_CLOSED = 0, TS_LISTEN, TS_SYN_SENT, TS_SYN_RCVD, TS_ESTABLISHED,
    TS_FIN_WAIT_1, TS_FIN_WAIT_2, TS_CLOSE_WAIT, TS_LAST_ACK, TS_CLOSING,
    TS_TIME_WAIT
};

struct tsock {
    bool used;
    int  state;
    u16  lport, rport;
    u32  rip;
    u32  iss, irs;
    u32  snd_una, snd_nxt, snd_wnd;
    u32  rcv_nxt;
    u16  mss;
    bool peer_fin;
    u32  rlen;
    u32  state_ms;
    u8   rbuf[TCP_RXBUF];
};

static struct tsock socks[TCP_MAX];
static u16 ephem = 40000;

static u64 irq_save(void)
{
    u64 f;
    __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}

static void irq_restore(u64 f)
{
    if (f & 0x200)
        __asm__ __volatile__("sti" ::: "memory");
    else
        __asm__ __volatile__("cli" ::: "memory");
}

static u32 tcp_isn(void)
{
    return timer_ticks() * 2654435761u + timer_uptime_ms() * 1103515245u;
}

static u16 next_port(void)
{
    for (int n = 0; n < 1000; n++) {
        u16 p = (u16)(ephem++ | 0xC000);
        bool used = false;
        for (int i = 0; i < TCP_MAX; i++)
            if (socks[i].used && socks[i].lport == p) { used = true; break; }
        if (!used)
            return p;
    }
    return 0;
}

static struct tsock *sock_get(int fd)
{
    if (fd < 1 || fd > TCP_MAX)
        return 0;
    struct tsock *s = &socks[fd - 1];
    return s->used ? s : 0;
}

int tcp_socket_count(void)
{
    int n = 0;
    for (int i = 0; i < TCP_MAX; i++)
        if (socks[i].used)
            n++;
    return n;
}

/* --------------------------------------------------------------- 发送 */
static void tcp_xmit(struct tsock *s, u8 flags, u32 seq, u32 ack,
                     const u8 *data, u32 len, bool opt_mss)
{
    u8 seg[1600];
    u32 hl = 20 + (opt_mss ? 4 : 0);
    if (hl + len > sizeof(seg))
        return;
    memset(seg, 0, hl);
    net_wr_be16(seg + 0, s->lport);
    net_wr_be16(seg + 2, s->rport);
    net_wr_be32(seg + 4, seq);
    net_wr_be32(seg + 8, ack);
    seg[12] = (u8)((hl / 4) << 4);
    seg[13] = flags;
    u32 wnd = TCP_WIN > s->rlen ? TCP_WIN - s->rlen : 0;
    net_wr_be16(seg + 14, (u16)wnd);
    net_wr_be16(seg + 16, 0);
    net_wr_be16(seg + 18, 0);
    if (opt_mss) {
        seg[20] = 2;
        seg[21] = 4;
        net_wr_be16(seg + 22, TCP_MSS);
    }
    if (len)
        memcpy(seg + hl, data, len);
    u32 total = hl + len;
    u32 src = net_ip();
    u32 sum = (src >> 16) + (src & 0xFFFF) +
              (s->rip >> 16) + (s->rip & 0xFFFF) + 6u + total;
    net_wr_be16(seg + 16, net_csum(seg, total, sum));
    net_send_ip(s->rip, 6, seg, total);
}

/* --------------------------------------------------------------- 输入 */
static struct tsock *sock_find(u32 src, u16 sport, u16 dport)
{
    for (int i = 0; i < TCP_MAX; i++) {
        struct tsock *s = &socks[i];
        if (!s->used || s->state == TS_LISTEN)
            continue;
        if (s->lport == dport && s->rport == sport && s->rip == src)
            return s;
    }
    for (int i = 0; i < TCP_MAX; i++) {
        struct tsock *s = &socks[i];
        if (s->used && s->state == TS_LISTEN && s->lport == dport)
            return s;
    }
    return 0;
}

static u16 tcp_parse_mss(const u8 *seg, u32 hl)
{
    u32 i = 20;
    while (i < hl) {
        u8 k = seg[i];
        if (k == 0)
            break;
        if (k == 1) { i++; continue; }
        if (i + 1 >= hl)
            break;
        u8 l = seg[i + 1];
        if (l < 2 || i + l > hl)
            break;
        if (k == 2 && l == 4)
            return net_be16(seg + i + 2);
        i += l;
    }
    return TCP_MSS;
}

void tcp_input(u32 src, u32 dst, const u8 *seg, u32 len)
{
    if (len < 20 || !net_link_up())
        return;
    u16 sport = net_be16(seg + 0);
    u16 dport = net_be16(seg + 2);
    u32 seq = net_be32(seg + 4);
    u32 ack = net_be32(seg + 8);
    u32 hl = (u32)(seg[12] >> 4) * 4;
    if (hl < 20 || hl > len)
        return;
    u8 flags = seg[13];
    u16 win = net_be16(seg + 14);

    u32 sum = (src >> 16) + (src & 0xFFFF) +
              (dst >> 16) + (dst & 0xFFFF) + 6u + len;
    if (net_csum(seg, len, sum) != 0)
        return;

    struct tsock *s = sock_find(src, sport, dport);
    if (!s)
        return;
    const u8 *data = seg + hl;
    u32 dlen = len - hl;

    if (flags & F_RST) {
        s->state = TS_CLOSED;
        return;
    }

    if (s->state == TS_LISTEN) {
        if (flags & F_SYN) {
            s->rip = src;
            s->rport = sport;
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->iss = tcp_isn();
            s->snd_una = s->iss;
            s->snd_nxt = s->iss + 1;
            s->snd_wnd = win ? win : TCP_MSS;
            s->mss = tcp_parse_mss(seg, hl);
            s->state = TS_SYN_RCVD;
            tcp_xmit(s, F_SYN | F_ACK, s->iss, s->rcv_nxt, 0, 0, true);
        }
        return;
    }

    if (s->state == TS_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->snd_nxt) {
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = win ? win : TCP_MSS;
            s->mss = tcp_parse_mss(seg, hl);
            s->state = TS_ESTABLISHED;
            tcp_xmit(s, F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
        }
        return;
    }

    if (s->state == TS_SYN_RCVD) {
        if (!(flags & F_ACK) || ack != s->snd_nxt)
            return;
        s->snd_una = ack;
        s->state = TS_ESTABLISHED;
    }

    if (flags & F_ACK) {
        if (ack > s->snd_una && ack <= s->snd_nxt)
            s->snd_una = ack;
        s->snd_wnd = win;
        if (s->state == TS_FIN_WAIT_1 && ack == s->snd_nxt) {
            s->state = TS_FIN_WAIT_2;
            s->state_ms = timer_uptime_ms();
        } else if (s->state == TS_LAST_ACK && ack == s->snd_nxt) {
            s->state = TS_CLOSED;
        } else if (s->state == TS_CLOSING && ack == s->snd_nxt) {
            s->state = TS_CLOSED;
        }
    }

    if (dlen) {
        if (seq == s->rcv_nxt && s->rlen + dlen <= TCP_RXBUF) {
            memcpy(s->rbuf + s->rlen, data, dlen);
            s->rlen += dlen;
            s->rcv_nxt += dlen;
        }
        tcp_xmit(s, F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
    }

    if (flags & F_FIN) {
        if (seq + dlen == s->rcv_nxt) {
            s->rcv_nxt++;
            s->peer_fin = true;
        }
        tcp_xmit(s, F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
        if (s->state == TS_ESTABLISHED)
            s->state = TS_CLOSE_WAIT;
        else if (s->state == TS_FIN_WAIT_1)
            s->state = TS_CLOSING;
        else if (s->state == TS_FIN_WAIT_2)
            s->state = TS_CLOSED;
    }
}

void tcp_poll(u32 now)
{
    for (int i = 0; i < TCP_MAX; i++) {
        struct tsock *s = &socks[i];
        if (!s->used)
            continue;
        if (s->state == TS_TIME_WAIT && now - s->state_ms > 1000) {
            s->used = false;
            s->state = TS_CLOSED;
        }
    }
}

/* --------------------------------------------------------------- API */
int tcp_connect(u32 ip, u16 port)
{
    if (!net_init() || ip == 0 || port == 0)
        return -1;
    int slot = -1;
    for (int i = 0; i < TCP_MAX; i++)
        if (!socks[i].used) { slot = i; break; }
    if (slot < 0)
        return -1;

    struct tsock *s = &socks[slot];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->state = TS_SYN_SENT;
    s->rip = ip;
    s->rport = port;
    s->lport = next_port();
    if (!s->lport) { s->used = false; return -1; }
    s->iss = tcp_isn();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->snd_wnd = TCP_MSS;
    s->mss = TCP_MSS;

    u64 fl = irq_save();
    cpu_sti();
    tcp_xmit(s, F_SYN, s->iss, 0, 0, 0, true);

    u32 t0 = timer_uptime_ms();
    u32 last = t0;
    int retry = 0;
    while (s->state == TS_SYN_SENT && timer_uptime_ms() - t0 < 3000) {
        net_poll();
        if (s->state != TS_SYN_SENT)
            break;
        if (timer_uptime_ms() - last > 700) {
            if (++retry > 4)
                break;
            tcp_xmit(s, F_SYN, s->iss, 0, 0, 0, true);
            last = timer_uptime_ms();
        }
        cpu_halt();
    }
    irq_restore(fl);

    if (s->state != TS_ESTABLISHED) {
        s->used = false;
        s->state = TS_CLOSED;
        return -1;
    }
    return slot + 1;
}

int tcp_send(int fd, const void *buf, u32 len)
{
    struct tsock *s = sock_get(fd);
    if (!s || s->state != TS_ESTABLISHED || !buf)
        return -1;
    const u8 *p = (const u8 *)buf;
    u32 sent = 0;
    u64 fl = irq_save();
    cpu_sti();

    while (sent < len) {
        u32 chunk = len - sent;
        if (chunk > s->mss)
            chunk = s->mss;
        if (s->snd_wnd && chunk > s->snd_wnd)
            chunk = s->snd_wnd;
        if (chunk == 0) {
            net_poll();
            if (s->state != TS_ESTABLISHED)
                break;
            cpu_halt();
            continue;
        }

        u32 seq = s->snd_nxt;
        tcp_xmit(s, F_ACK | F_PSH, seq, s->rcv_nxt, p + sent, chunk, false);
        s->snd_nxt += chunk;

        u32 t0 = timer_uptime_ms();
        int retry = 0;
        while (s->snd_una < s->snd_nxt && s->state == TS_ESTABLISHED) {
            net_poll();
            if (s->snd_una >= s->snd_nxt || s->state != TS_ESTABLISHED)
                break;
            if (timer_uptime_ms() - t0 > 600) {
                if (++retry > 5)
                    break;
                tcp_xmit(s, F_ACK | F_PSH, seq, s->rcv_nxt, p + sent, chunk, false);
                t0 = timer_uptime_ms();
            }
            cpu_halt();
        }
        if (s->snd_una < s->snd_nxt || s->state != TS_ESTABLISHED)
            break;
        sent += chunk;
    }

    irq_restore(fl);
    return (int)sent;
}

int tcp_recv(int fd, void *buf, u32 len)
{
    struct tsock *s = sock_get(fd);
    if (!s || !buf)
        return -1;
    if (s->state != TS_ESTABLISHED && s->state != TS_CLOSE_WAIT &&
        s->state != TS_FIN_WAIT_2)
        return 0;

    u64 fl = irq_save();
    cpu_sti();
    u32 t0 = timer_uptime_ms();
    while (s->rlen == 0 && !s->peer_fin &&
           (s->state == TS_ESTABLISHED || s->state == TS_CLOSE_WAIT ||
            s->state == TS_FIN_WAIT_2) &&
           timer_uptime_ms() - t0 < 3000) {
        net_poll();
        if (s->rlen || s->peer_fin)
            break;
        cpu_halt();
    }

    u32 k = len;
    if (k > s->rlen)
        k = s->rlen;
    if (k) {
        memcpy(buf, s->rbuf, k);
        memmove(s->rbuf, s->rbuf + k, s->rlen - k);
        s->rlen -= k;
        tcp_xmit(s, F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
    }
    irq_restore(fl);
    return (int)k;
}

int tcp_close(int fd)
{
    struct tsock *s = sock_get(fd);
    if (!s)
        return -1;

    u64 fl = irq_save();
    cpu_sti();
    if (s->state == TS_ESTABLISHED) {
        tcp_xmit(s, F_FIN | F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
        s->snd_nxt++;
        s->state = TS_FIN_WAIT_1;
    } else if (s->state == TS_CLOSE_WAIT) {
        tcp_xmit(s, F_FIN | F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
        s->snd_nxt++;
        s->state = TS_LAST_ACK;
    } else if (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD) {
        s->state = TS_CLOSED;
    }

    u32 t0 = timer_uptime_ms();
    u32 last = t0;
    while (s->state != TS_CLOSED && s->state != TS_TIME_WAIT &&
           timer_uptime_ms() - t0 < 1500) {
        net_poll();
        if (s->state == TS_CLOSED || s->state == TS_TIME_WAIT)
            break;
        if (s->state == TS_FIN_WAIT_1 && timer_uptime_ms() - last > 700) {
            tcp_xmit(s, F_FIN | F_ACK, s->snd_nxt - 1, s->rcv_nxt, 0, 0, false);
            last = timer_uptime_ms();
        }
        cpu_halt();
    }
    s->used = false;
    s->state = TS_CLOSED;
    irq_restore(fl);
    return 0;
}

int tcp_listen(u16 port)
{
    if (!net_init() || port == 0)
        return -1;
    int slot = -1;
    for (int i = 0; i < TCP_MAX; i++)
        if (!socks[i].used) { slot = i; break; }
    if (slot < 0)
        return -1;
    struct tsock *s = &socks[slot];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->state = TS_LISTEN;
    s->lport = port;
    s->mss = TCP_MSS;
    return slot + 1;
}

int tcp_accept(int fd)
{
    struct tsock *s = sock_get(fd);
    if (!s || s->state != TS_LISTEN)
        return -1;
    u64 fl = irq_save();
    cpu_sti();
    u32 t0 = timer_uptime_ms();
    while (s->state != TS_ESTABLISHED && timer_uptime_ms() - t0 < 10000) {
        net_poll();
        if (s->state == TS_ESTABLISHED)
            break;
        cpu_halt();
    }
    irq_restore(fl);
    return s->state == TS_ESTABLISHED ? 0 : -1;
}
