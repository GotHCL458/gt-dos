/* TCP: 主动连接/被动监听, 数据收发, 超时重传, 四次挥手.
   完整实现包含: 乱序段排队重组 (OOO), 三次重复 ACK 快速重传,
   慢启动/拥塞避免/快速恢复, 以及 RST 的收发与上报.
   阻塞式 socket 表, 由 INT 0x21 / shell / netapp 调用.
   收包经 net_poll() -> tcp_input(); 重传与等待都用轮询. */
#include "gt.h"

#define TCP_MAX     8
#define TCP_RXBUF   4096
#define TCP_SNDBUF  5840                /* 4*MSS: 够 3 个后续段触发快速重传 */
#define TCP_MSS     1460
#define TCP_WIN     TCP_RXBUF
#define TCP_OOO     4                   /* 每个 socket 的乱序段缓存 */
#define TCP_RTO_MIN 500
#define TCP_RTO_MAX 4000
#define TCP_CWND_MAX (8 * TCP_MSS)

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
    u32  cwnd, ssthresh;                /* 拥塞控制 */
    u32  dupacks;                       /* 连续重复 ACK 计数 */
    u32  rto, last_ack_ms;
    u32  rcv_nxt;
    u16  mss;
    bool peer_fin;
    bool in_recovery;
    u8   err;                           /* 1=对端 RST, 2=ICMP 不可达 */
    u32  rlen;
    u32  state_ms;
    u32  snd_len;                       /* sndbuf 中未确认字节数 */
    u8   rbuf[TCP_RXBUF];
    u8   sndbuf[TCP_SNDBUF];
    struct {
        bool used;
        u32  seq;
        u16  len;
        u8   data[TCP_MSS];
    } ooo[TCP_OOO];
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

/* 重传 sndbuf 首段 (snd_una 起) */
static void tcp_retransmit(struct tsock *s)
{
    if (!s->snd_len)
        return;
    u32 n = s->snd_len > s->mss ? s->mss : s->snd_len;
    tcp_xmit(s, (u8)(F_ACK | F_PSH), s->snd_una, s->rcv_nxt, s->sndbuf, n, false);
}

/* 无端口匹配时回 RST (RFC 793 简化处理) */
static void tcp_send_rst(u32 dst, u16 lport, u16 rport, u32 seq, u32 ack,
                         bool with_ack)
{
    u8 seg[20];
    memset(seg, 0, sizeof(seg));
    net_wr_be16(seg + 0, lport);
    net_wr_be16(seg + 2, rport);
    net_wr_be32(seg + 4, seq);
    net_wr_be32(seg + 8, ack);
    seg[12] = (u8)((20 / 4) << 4);
    seg[13] = (u8)(F_RST | (with_ack ? F_ACK : 0));
    u32 src = net_ip();
    u32 sum = (src >> 16) + (src & 0xFFFF) +
              (dst >> 16) + (dst & 0xFFFF) + 6u + 20u;
    net_wr_be16(seg + 16, net_csum(seg, 20, sum));
    net_send_ip(dst, 6, seg, 20);
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

static void tcp_append(struct tsock *s, const u8 *d, u32 n)
{
    if (s->rlen + n > TCP_RXBUF)
        n = TCP_RXBUF - s->rlen;
    if (!n)
        return;
    memcpy(s->rbuf + s->rlen, d, n);
    s->rlen += n;
    s->rcv_nxt += n;
}

/* 按序数据入队, 并从乱序队列里接续可拼的段 */
static void tcp_deliver_inorder(struct tsock *s, const u8 *d, u32 n)
{
    tcp_append(s, d, n);
    for (bool progress = true; progress; ) {
        progress = false;
        for (int i = 0; i < TCP_OOO; i++) {
            if (!s->ooo[i].used)
                continue;
            u32 seq = s->ooo[i].seq;
            u32 len = s->ooo[i].len;
            if (seq == s->rcv_nxt) {
                tcp_append(s, s->ooo[i].data, len);
                s->ooo[i].used = false;
                progress = true;
            } else if (seq < s->rcv_nxt && seq + len > s->rcv_nxt) {
                u32 skip = s->rcv_nxt - seq;
                tcp_append(s, s->ooo[i].data + skip, len - skip);
                s->ooo[i].used = false;
                progress = true;
            }
        }
    }
}

static void tcp_store_ooo(struct tsock *s, u32 seq, const u8 *d, u32 n)
{
    if (n > TCP_MSS)
        n = TCP_MSS;
    int slot = -1;
    for (int i = 0; i < TCP_OOO; i++) {
        if (s->ooo[i].used && s->ooo[i].seq == seq) {
            slot = i;                    /* 重复段: 覆盖 */
            break;
        }
        if (!s->ooo[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return;                          /* 队列满: 丢弃, 等对端重传 */
    s->ooo[slot].used = true;
    s->ooo[slot].seq = seq;
    s->ooo[slot].len = (u16)n;
    memcpy(s->ooo[slot].data, d, n);
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

    const u8 *data = seg + hl;
    u32 dlen = len - hl;
    u32 now = timer_uptime_ms();

    struct tsock *s = sock_find(src, sport, dport);
    if (!s) {
        /* 没有匹配的连接: 按 RFC 793 回 RST, 避免半开连接 */
        if (!(flags & F_RST)) {
            if (flags & F_ACK)
                tcp_send_rst(src, dport, sport, ack, 0, false);
            else
                tcp_send_rst(src, dport, sport, 0,
                             seq + dlen + ((flags & F_SYN) ? 1u : 0u) +
                             ((flags & F_FIN) ? 1u : 0u), true);
        }
        return;
    }

    if (flags & F_RST) {
        if (s->state != TS_LISTEN) {
            s->state = TS_CLOSED;
            s->err = 1;
            s->snd_len = 0;
            s->dupacks = 0;
        }
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
            s->cwnd = s->mss;
            s->ssthresh = 4 * (u32)s->mss;
            s->dupacks = 0;
            s->err = 0;
            s->in_recovery = false;
            s->state = TS_SYN_RCVD;
            tcp_xmit(s, F_SYN | F_ACK, s->iss, s->rcv_nxt, 0, 0, true);
            s->last_ack_ms = now;
            s->rto = TCP_RTO_MIN;
        } else {
            tcp_send_rst(src, dport, sport, ack, 0, true);
        }
        return;
    }

    if (s->state == TS_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->snd_nxt) {
            s->irs = seq;
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_nxt = ack;
            s->snd_wnd = win ? win : TCP_MSS;
            s->mss = tcp_parse_mss(seg, hl);
            s->cwnd = s->mss;
            s->ssthresh = 4 * (u32)s->mss;
            s->dupacks = 0;
            s->in_recovery = false;
            s->state = TS_ESTABLISHED;
            s->last_ack_ms = now;
            s->rto = TCP_RTO_MIN;
            tcp_xmit(s, F_ACK, s->snd_nxt, s->rcv_nxt, 0, 0, false);
        }
        return;
    }

    if (s->state == TS_SYN_RCVD) {
        if (!(flags & F_ACK) || ack != s->snd_nxt)
            return;
        s->snd_una = ack;
        s->state = TS_ESTABLISHED;
        s->last_ack_ms = now;
    }

    /* ---------------------------------------------------- ACK 与拥塞控制 */
    if (flags & F_ACK) {
        if (ack > s->snd_una && ack <= s->snd_nxt) {
            u32 acked = ack - s->snd_una;
            if (acked > s->snd_len)
                acked = s->snd_len;
            if (acked) {
                memmove(s->sndbuf, s->sndbuf + acked, s->snd_len - acked);
                s->snd_len -= acked;
                s->snd_una = ack;
                s->snd_nxt = s->snd_una + s->snd_len;
                s->dupacks = 0;
                s->last_ack_ms = now;
                s->rto = TCP_RTO_MIN;
                if (s->in_recovery) {
                    s->cwnd = s->ssthresh;             /* 快速恢复结束 */
                    s->in_recovery = false;
                } else if (s->cwnd < s->ssthresh) {
                    s->cwnd += s->mss;                 /* 慢启动 */
                } else {
                    u32 inc = (u32)s->mss * s->mss / s->cwnd;
                    s->cwnd += inc ? inc : 1;          /* 拥塞避免 */
                }
                if (s->cwnd > TCP_CWND_MAX)
                    s->cwnd = TCP_CWND_MAX;
            }
        } else if (ack == s->snd_una && s->snd_len && dlen == 0 &&
                   !(flags & (F_SYN | F_FIN)) && win == s->snd_wnd) {
            if (!s->in_recovery) {
                if (++s->dupacks == 3) {
                    /* 三次重复 ACK: 快速重传 + 快速恢复 */
                    s->ssthresh = (s->cwnd / 2 / s->mss) * s->mss;
                    if (s->ssthresh < 2 * s->mss)
                        s->ssthresh = 2 * s->mss;
                    s->cwnd = s->ssthresh + 3 * (u32)s->mss;
                    s->in_recovery = true;
                    s->last_ack_ms = now;
                    s->rto = TCP_RTO_MIN;
                    tcp_retransmit(s);
                }
            } else {
                s->cwnd += s->mss;
                if (s->cwnd > TCP_CWND_MAX)
                    s->cwnd = TCP_CWND_MAX;
            }
        }
        s->snd_wnd = win;

        if (s->state == TS_FIN_WAIT_1 && ack == s->snd_nxt) {
            s->state = TS_FIN_WAIT_2;
            s->state_ms = now;
        } else if (s->state == TS_LAST_ACK && ack == s->snd_nxt) {
            s->state = TS_CLOSED;
        } else if (s->state == TS_CLOSING && ack == s->snd_nxt) {
            s->state = TS_CLOSED;
        }
    }

    /* -------------------------------------------------------- 数据接收 */
    if (dlen) {
        if (seq + dlen <= s->rcv_nxt) {
            /* 完全重复: 只需 ACK */
        } else {
            u32 skip = 0;
            if ((i32)(seq - s->rcv_nxt) < 0)
                skip = s->rcv_nxt - seq;
            u32 useq = seq + skip;
            const u8 *ud = data + skip;
            u32 ulen = dlen - skip;
            if (useq == s->rcv_nxt)
                tcp_deliver_inorder(s, ud, ulen);
            else
                tcp_store_ooo(s, useq, ud, ulen);
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
        else if (s->state == TS_FIN_WAIT_2) {
            s->state = TS_TIME_WAIT;
            s->state_ms = now;
        }
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
            continue;
        }
        /* 兜底重传: 未被 tcp_send 阻塞覆盖的未确认数据 */
        if (s->snd_len && s->state != TS_CLOSED && s->state != TS_LISTEN &&
            now - s->last_ack_ms > s->rto) {
            s->ssthresh = (s->cwnd / 2 / s->mss) * s->mss;
            if (s->ssthresh < 2 * s->mss)
                s->ssthresh = 2 * s->mss;
            s->cwnd = s->mss;
            s->in_recovery = false;
            tcp_retransmit(s);
            s->last_ack_ms = now;
            s->rto = s->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2;
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
    s->cwnd = TCP_MSS;
    s->ssthresh = 4 * (u32)TCP_MSS;
    s->rto = TCP_RTO_MIN;

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

    if (s->state != TS_ESTABLISHED || s->err) {
        s->used = false;
        s->state = TS_CLOSED;
        return -1;
    }
    return slot + 1;
}

int tcp_send(int fd, const void *buf, u32 len)
{
    struct tsock *s = sock_get(fd);
    if (!s || !buf)
        return -1;
    if (s->err)
        return -1;
    if (s->state != TS_ESTABLISHED)
        return -1;
    if (len == 0)
        return 0;

    const u8 *p = (const u8 *)buf;
    u32 off = 0;
    int timeouts = 0;
    u64 fl = irq_save();
    cpu_sti();

    while (off < len || s->snd_len) {
        if (s->state != TS_ESTABLISHED || s->err)
            break;

        /* 在拥塞窗口/对端窗口允许范围内尽量多发 */
        u32 limit = s->snd_wnd;
        u32 cw = s->cwnd ? s->cwnd : s->mss;
        if (cw < limit)
            limit = cw;
        if (limit > TCP_SNDBUF)
            limit = TCP_SNDBUF;

        while (off < len && s->snd_len < limit) {
            u32 n = len - off;
            if (n > s->mss)
                n = s->mss;
            if (n > TCP_SNDBUF - s->snd_len)
                n = TCP_SNDBUF - s->snd_len;
            if (n > limit - s->snd_len)
                n = limit - s->snd_len;
            if (!n)
                break;
            memcpy(s->sndbuf + s->snd_len, p + off, n);
            tcp_xmit(s, (u8)(F_ACK | F_PSH), s->snd_una + s->snd_len,
                     s->rcv_nxt, s->sndbuf + s->snd_len, n, false);
            s->snd_len += n;
            s->snd_nxt = s->snd_una + s->snd_len;
            off += n;
        }

        if (!s->snd_len && off >= len)
            break;

        /* 等待 ACK 推进 (快速重传在 tcp_input 中处理) */
        u32 before = s->snd_una;
        u32 t0 = timer_uptime_ms();
        while (s->snd_una == before && s->state == TS_ESTABLISHED && !s->err &&
               timer_uptime_ms() - t0 < TCP_RTO_MAX) {
            net_poll();
            if (s->snd_una != before || s->state != TS_ESTABLISHED || s->err)
                break;
            cpu_halt();
        }
        if (s->snd_una == before) {
            if (++timeouts > 5)
                break;
            /* 超时: 拥塞窗口收缩 + 指数退避后重传 */
            s->ssthresh = (s->cwnd / 2 / s->mss) * s->mss;
            if (s->ssthresh < 2 * s->mss)
                s->ssthresh = 2 * s->mss;
            s->cwnd = s->mss;
            s->in_recovery = false;
            s->rto = s->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2;
            tcp_retransmit(s);
            s->last_ack_ms = timer_uptime_ms();
        }
    }

    if (s->err) {
        irq_restore(fl);
        return -1;
    }
    irq_restore(fl);
    return (int)off;
}

int tcp_recv(int fd, void *buf, u32 len)
{
    struct tsock *s = sock_get(fd);
    if (!s || !buf)
        return -1;
    if (s->err)
        return -1;
    if (s->state != TS_ESTABLISHED && s->state != TS_CLOSE_WAIT &&
        s->state != TS_FIN_WAIT_2)
        return 0;

    u64 fl = irq_save();
    cpu_sti();
    u32 t0 = timer_uptime_ms();
    while (s->rlen == 0 && !s->peer_fin && !s->err &&
           (s->state == TS_ESTABLISHED || s->state == TS_CLOSE_WAIT ||
            s->state == TS_FIN_WAIT_2) &&
           timer_uptime_ms() - t0 < 3000) {
        net_poll();
        if (s->rlen || s->peer_fin || s->err)
            break;
        cpu_halt();
    }

    if (s->err && s->rlen == 0) {
        irq_restore(fl);
        return -1;
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
    u32 fseq = s->snd_una + s->snd_len;
    if (s->state == TS_ESTABLISHED) {
        tcp_xmit(s, F_FIN | F_ACK, fseq, s->rcv_nxt, 0, 0, false);
        s->snd_nxt = fseq + 1;
        s->state = TS_FIN_WAIT_1;
    } else if (s->state == TS_CLOSE_WAIT) {
        tcp_xmit(s, F_FIN | F_ACK, fseq, s->rcv_nxt, 0, 0, false);
        s->snd_nxt = fseq + 1;
        s->state = TS_LAST_ACK;
    } else if (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD) {
        s->state = TS_CLOSED;
    }

    u32 t0 = timer_uptime_ms();
    u32 last = t0;
    while (s->state != TS_CLOSED && s->state != TS_TIME_WAIT &&
           s->err == 0 && timer_uptime_ms() - t0 < 1500) {
        net_poll();
        if (s->state == TS_CLOSED || s->state == TS_TIME_WAIT || s->err)
            break;
        if (s->state == TS_FIN_WAIT_1 && timer_uptime_ms() - last > 700) {
            tcp_xmit(s, F_FIN | F_ACK, s->snd_nxt - 1, s->rcv_nxt, 0, 0, false);
            last = timer_uptime_ms();
        }
        cpu_halt();
    }
    s->used = false;
    s->state = TS_CLOSED;
    s->snd_len = 0;
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
    s->cwnd = TCP_MSS;
    s->ssthresh = 4 * (u32)TCP_MSS;
    s->rto = TCP_RTO_MIN;
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
    while (s->state != TS_ESTABLISHED && !s->err &&
           timer_uptime_ms() - t0 < 10000) {
        net_poll();
        if (s->state == TS_ESTABLISHED || s->err)
            break;
        cpu_halt();
    }
    irq_restore(fl);
    return (s->state == TS_ESTABLISHED && !s->err) ? 0 : -1;
}

/* ICMP 不可达: 标记匹配的连接, 让 send/recv 返回 -1 */
void tcp_icmp_error(u16 lport, u32 peer)
{
    for (int i = 0; i < TCP_MAX; i++) {
        struct tsock *s = &socks[i];
        if (!s->used || s->state == TS_LISTEN)
            continue;
        if (s->lport == lport && s->rip == peer)
            s->err = 2;
    }
}
