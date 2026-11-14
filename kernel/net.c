/* 网络栈底层: RTL8139 驱动 + Ethernet/ARP/IPv4/ICMP/UDP/DHCP.
   轮询方式收包 (不开网卡中断), TCP 在 tcp.c.
   地址统一用"网络字节序数值"表示, 如 10.0.2.2 = 0x0A000202. */
#include "gt.h"

/* ------------------------------------------------------------ RTL8139 */
#define RTL_IDR0    0x00
#define RTL_TSD0    0x10
#define RTL_TSAD0   0x20
#define RTL_RBSTART 0x30
#define RTL_CR      0x37
#define RTL_CAPR    0x38
#define RTL_IMR     0x3C
#define RTL_ISR     0x3E
#define RTL_TCR     0x40
#define RTL_RCR     0x44
#define RTL_CONFIG1 0x52

#define RX_RING     8192
#define RX_PAD      16
#define RX_TOTAL    (RX_RING + RX_PAD + 1600)

static u16 io_base;
static u8  dev_mac[6];
static bool link_up;
static bool inited;

static u32 ip_addr, ip_mask, ip_gw, ip_dns;
static u32 ip_ident;
static u32 rx_off;
static int tx_idx;

static u8 rx_buf[RX_TOTAL] __attribute__((section(".highbss"), aligned(16)));
static u8 tx_buf[4][1600] __attribute__((section(".highbss"), aligned(16)));
static u8 tx_frame[1600] __attribute__((section(".highbss"), aligned(16)));

static void net_handle_frame(const u8 *f, u32 n);
static void ip_input(const u8 *p, u32 n);
static void arp_input(const u8 *p, u32 n);

/* ---------------------------------------------------------- 工具函数 */
u16 net_be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
u32 net_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
void net_wr_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
void net_wr_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

u16 net_csum(const u8 *p, u32 len, u32 sum)
{
    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

bool net_parse_ip(const char *s, u32 *out)
{
    u32 parts[4] = {0, 0, 0, 0};
    int n = 0;
    u32 v = 0;
    bool any = false;
    for (;;) {
        char c = *s++;
        if (c >= '0' && c <= '9') {
            v = v * 10 + (u32)(c - '0');
            any = true;
        } else if (c == '.' || c == 0) {
            if (!any || v > 255 || n >= 4)
                return false;
            parts[n++] = v;
            v = 0;
            any = false;
            if (c == 0)
                break;
        } else {
            return false;
        }
    }
    if (n != 4)
        return false;
    *out = IP4(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

void net_ip_str(u32 ip, char *buf)
{
    char tmp[16];
    int k = 0;
    for (int i = 3; i >= 0; i--) {
        u32 v = (ip >> (i * 8)) & 0xFF;
        itoa_base(v, tmp, 10, false);
        for (char *p = tmp; *p; p++)
            buf[k++] = *p;
        if (i)
            buf[k++] = '.';
    }
    buf[k] = 0;
}

/* --------------------------------------------------------------- PCI */
static u32 pci_rd(u8 slot, u8 off)
{
    outl(0xCF8, 0x80000000u | ((u32)slot << 11) | ((u32)off & 0xFC));
    return inl(0xCFC);
}

static void pci_wr(u8 slot, u8 off, u32 v)
{
    outl(0xCF8, 0x80000000u | ((u32)slot << 11) | ((u32)off & 0xFC));
    outl(0xCFC, v);
}

static u16 rtl_probe(void)
{
    for (u8 slot = 1; slot < 32; slot++) {
        u32 id = pci_rd(slot, 0);
        if ((id & 0xFFFF) != 0x10EC || (id >> 16) != 0x8139)
            continue;
        u32 bar = pci_rd(slot, 0x10);
        if (!(bar & 1))
            continue;
        u32 cmd = pci_rd(slot, 0x04);
        pci_wr(slot, 0x04, cmd | 0x05);      /* I/O space + bus master */
        return (u16)(bar & ~3u);
    }
    return 0;
}

static bool rtl_send(const u8 *data, u32 len)
{
    if (!link_up || len > 1600)
        return false;
    int i = tx_idx;
    tx_idx = (tx_idx + 1) & 3;
    memcpy(tx_buf[i], data, len);
    __asm__ __volatile__("" ::: "memory");
    outl(io_base + RTL_TSAD0 + i * 4, (u32)(u64)tx_buf[i]);
    outl(io_base + RTL_TSD0 + i * 4, len & 0x1FFF);
    for (u32 n = 0; n < 2000000; n++)
        if (inl(io_base + RTL_TSD0 + i * 4) & 0x8000)
            return true;
    return false;
}

static void rtl_poll(void)
{
    if (!link_up)
        return;
    for (int guard = 0; guard < 64; guard++) {
        if (inb(io_base + RTL_CR) & 0x01)       /* BUFE: 空 */
            break;
        u32 off = rx_off;
        u16 status = (u16)(rx_buf[off] | (rx_buf[off + 1] << 8));
        u16 len = (u16)(rx_buf[off + 2] | (rx_buf[off + 3] << 8));
        if (!(status & 0x01) || len < 4 || len > 1600) {
            rx_off = 0;
            outl(io_base + RTL_CAPR, 0);
            break;
        }
        net_handle_frame(rx_buf + off + 4, (u32)(len - 4));
        rx_off = (off + (u32)len + 4 + 3) & ~3u;
        if (rx_off >= RX_RING)
            rx_off -= RX_RING;
        outl(io_base + RTL_CAPR, (rx_off - RX_PAD) & 0xFFFF);
    }
}

bool net_init(void)
{
    if (inited)
        return link_up;
    inited = true;
    io_base = rtl_probe();
    if (!io_base)
        return false;

    outb(io_base + RTL_CONFIG1, 0x00);
    outb(io_base + RTL_CR, 0x10);              /* 复位 */
    for (u32 i = 0; i < 100000 && (inb(io_base + RTL_CR) & 0x10); i++)
        ;
    for (int i = 0; i < 6; i++)
        dev_mac[i] = inb(io_base + RTL_IDR0 + i);

    outl(io_base + RTL_RBSTART, (u32)(u64)rx_buf);
    outl(io_base + RTL_RCR, 0x0000008F);       /* WRAP + 接收全部 */
    outl(io_base + RTL_TCR, 0x00000700);
    outw(io_base + RTL_IMR, 0x0000);
    outw(io_base + RTL_ISR, 0xFFFF);
    outb(io_base + RTL_CR, 0x0C);              /* RE | TE */

    rx_off = 0;
    tx_idx = 0;
    link_up = true;

    ip_addr = IP4(10, 0, 2, 15);
    ip_mask = IP4(255, 255, 255, 0);
    ip_gw = IP4(10, 0, 2, 2);
    ip_dns = IP4(10, 0, 2, 3);
    ip_ident = (u32)(timer_ticks() & 0xFFFF);
    return true;
}

bool net_link_up(void) { return link_up; }
u32 net_ip(void) { return ip_addr; }
u32 net_mask(void) { return ip_mask; }
u32 net_gw(void) { return ip_gw; }
u32 net_dns(void) { return ip_dns; }

void net_get_info(struct net_info *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->ip = ip_addr;
    out->mask = ip_mask;
    out->gw = ip_gw;
    out->dns = ip_dns;
    memcpy(out->mac, dev_mac, 6);
    out->link = link_up ? 1 : 0;
    out->mtu = 1500;
}

void net_poll(void)
{
    if (link_up)
        rtl_poll();
    tcp_poll(timer_uptime_ms());
}

/* --------------------------------------------------------------- ARP */
struct arp_ent { u32 ip; u8 mac[6]; bool valid; };
static struct arp_ent arp_tab[8];
static const u8 bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool arp_lookup(u32 ip, u8 mac[6])
{
    for (int i = 0; i < 8; i++) {
        if (arp_tab[i].valid && arp_tab[i].ip == ip) {
            memcpy(mac, arp_tab[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_store(u32 ip, const u8 *mac)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (arp_tab[i].valid && arp_tab[i].ip == ip) { slot = i; goto set; }
        if (!arp_tab[i].valid) { slot = i; break; }
    }
set:
    arp_tab[slot].valid = true;
    arp_tab[slot].ip = ip;
    memcpy(arp_tab[slot].mac, mac, 6);
}

static void arp_send(u16 op, u32 target_ip, const u8 *target_mac)
{
    u8 f[42];
    memcpy(f, target_mac, 6);
    memcpy(f + 6, dev_mac, 6);
    net_wr_be16(f + 12, 0x0806);
    net_wr_be16(f + 14, 1);          /* htype */
    net_wr_be16(f + 16, 0x0800);     /* ptype */
    f[18] = 6;
    f[19] = 4;
    net_wr_be16(f + 20, op);
    memcpy(f + 22, dev_mac, 6);
    net_wr_be32(f + 28, ip_addr);
    memcpy(f + 32, target_mac, 6);
    net_wr_be32(f + 38, target_ip);
    rtl_send(f, 42);
}

static void arp_input(const u8 *p, u32 n)
{
    if (n < 28)
        return;
    if (net_be16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4)
        return;
    u16 op = net_be16(p + 6);
    u32 spa = net_be32(p + 14);
    u32 tpa = net_be32(p + 24);
    if (op == 1 && tpa == ip_addr) {
        u8 f[42];
        memcpy(f, p + 8, 6);
        memcpy(f + 6, dev_mac, 6);
        net_wr_be16(f + 12, 0x0806);
        net_wr_be16(f + 14, 1);
        net_wr_be16(f + 16, 0x0800);
        f[18] = 6;
        f[19] = 4;
        net_wr_be16(f + 20, 2);
        memcpy(f + 22, dev_mac, 6);
        net_wr_be32(f + 28, ip_addr);
        memcpy(f + 32, p + 8, 6);
        net_wr_be32(f + 38, spa);
        rtl_send(f, 42);
    } else if (op == 2) {
        arp_store(spa, p + 8);
    }
}

static bool arp_resolve(u32 ip, u8 mac[6])
{
    if (ip == 0xFFFFFFFFu) {
        memcpy(mac, bcast_mac, 6);
        return true;
    }
    if (arp_lookup(ip, mac))
        return true;
    arp_send(1, ip, bcast_mac);
    u32 t0 = timer_uptime_ms();
    while (timer_uptime_ms() - t0 < 800) {
        net_poll();
        if (arp_lookup(ip, mac))
            return true;
        cpu_halt();
    }
    return false;
}

/* ------------------------------------------------------------ IPv4 */
static bool ip_send_ex(u32 src, u32 dst, u8 proto, const void *payload, u32 plen)
{
    if (!link_up || plen > 1480)
        return false;
    u8 hop[6];
    if (dst == 0xFFFFFFFFu) {
        memcpy(hop, bcast_mac, 6);
    } else {
        u32 nh = ((dst & ip_mask) == (ip_addr & ip_mask)) ? dst : ip_gw;
        if (!arp_resolve(nh, hop))
            return false;
    }
    u8 *f = tx_frame;
    memcpy(f, hop, 6);
    memcpy(f + 6, dev_mac, 6);
    net_wr_be16(f + 12, 0x0800);
    u8 *ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    net_wr_be16(ip + 2, (u16)(20 + plen));
    net_wr_be16(ip + 4, (u16)(++ip_ident));
    net_wr_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    net_wr_be16(ip + 10, 0);
    net_wr_be32(ip + 12, src);
    net_wr_be32(ip + 16, dst);
    net_wr_be16(ip + 10, net_csum(ip, 20, 0));
    memcpy(ip + 20, payload, plen);
    return rtl_send(f, 14 + 20 + plen);
}

bool net_send_ip(u32 dst, u8 proto, const void *payload, u32 len)
{
    return ip_send_ex(ip_addr, dst, proto, payload, len);
}

/* ------------------------------------------------------------ ICMP */
static volatile bool ping_wait;
static volatile u32 ping_ip;
static volatile u16 ping_id, ping_seq;
static volatile u32 ping_ms;

static void icmp_input(u32 src, const u8 *p, u32 n)
{
    if (n < 8 || net_csum(p, n, 0) != 0)
        return;
    if (p[0] == 8) {                       /* echo request */
        u8 rep[1500];
        if (n > sizeof(rep))
            return;
        memcpy(rep, p, n);
        rep[0] = 0;
        net_wr_be16(rep + 2, 0);
        net_wr_be16(rep + 2, net_csum(rep, n, 0));
        net_send_ip(src, 1, rep, n);
    } else if (p[0] == 0) {                /* echo reply */
        if (!ping_wait || src != ping_ip)
            return;
        if (net_be16(p + 4) == ping_id && net_be16(p + 6) == ping_seq) {
            ping_ms = timer_uptime_ms();
            ping_wait = false;
        }
    }
}

int net_ping_one(u32 ip, u16 seq, u32 *rtt_ms)
{
    if (!net_init())
        return 0;
    u8 req[64];
    memset(req, 0, sizeof(req));
    req[0] = 8;
    ping_id = (u16)(0x4754u ^ (timer_ticks() & 0xFFFF));
    ping_seq = seq;
    ping_ip = ip;
    net_wr_be16(req + 4, ping_id);
    net_wr_be16(req + 6, seq);
    net_wr_be16(req + 2, net_csum(req, sizeof(req), 0));

    ping_wait = true;
    u32 t0 = timer_uptime_ms();
    if (!net_send_ip(ip, 1, req, sizeof(req))) {
        ping_wait = false;
        return 0;
    }
    while (ping_wait && timer_uptime_ms() - t0 < 1000) {
        net_poll();
        if (!ping_wait)
            break;
        cpu_halt();
    }
    if (ping_wait)
        return 0;
    if (rtt_ms)
        *rtt_ms = ping_ms - t0;
    return 1;
}

/* ------------------------------------------------------ UDP / DHCP */
static bool dhcp_active;
static u32 dhcp_xid;
static u8  dhcp_offer_type;
static u32 dhcp_yiaddr, dhcp_mask, dhcp_gw, dhcp_dns, dhcp_server;

static void dhcp_input(const u8 *p, u32 n, u16 dport)
{
    if (!dhcp_active || dport != 68 || n < 240)
        return;
    if (p[0] != 2 || net_be32(p + 4) != dhcp_xid)
        return;
    if (net_be32(p + 236) != 0x63825363u)
        return;
    dhcp_yiaddr = net_be32(p + 16);
    const u8 *o = p + 240;
    u32 left = n - 240;
    while (left > 0) {
        u8 t = o[0];
        if (t == 255)
            break;
        if (t == 0) { o++; left--; continue; }
        if (left < 2)
            break;
        u8 l = o[1];
        if ((u32)l + 2 > left)
            break;
        const u8 *v = o + 2;
        if (t == 53 && l >= 1)
            dhcp_offer_type = v[0];
        else if (t == 1 && l >= 4)
            dhcp_mask = net_be32(v);
        else if (t == 3 && l >= 4)
            dhcp_gw = net_be32(v);
        else if (t == 6 && l >= 4)
            dhcp_dns = net_be32(v);
        else if (t == 54 && l >= 4)
            dhcp_server = net_be32(v);
        o += 2 + l;
        left -= 2u + l;
    }
}

static void udp_input(u32 src, const u8 *p, u32 n)
{
    (void)src;
    if (n < 8)
        return;
    u16 dport = net_be16(p + 2);
    u32 len = net_be16(p + 4);
    if (len < 8 || len > n)
        len = n;
    dhcp_input(p + 8, len - 8, dport);
}

static bool dhcp_send(u8 type, u32 req_ip)
{
    u8 pkt[300];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 1;                 /* BOOTREQUEST */
    pkt[1] = 1;                 /* Ethernet */
    pkt[2] = 6;
    net_wr_be32(pkt + 4, dhcp_xid);
    net_wr_be16(pkt + 10, 0x8000);
    memcpy(pkt + 28, dev_mac, 6);
    net_wr_be32(pkt + 236, 0x63825363u);

    u32 o = 240;
    pkt[o++] = 53; pkt[o++] = 1; pkt[o++] = type;
    if (type == 3) {
        pkt[o++] = 50; pkt[o++] = 4;
        net_wr_be32(pkt + o, req_ip); o += 4;
        pkt[o++] = 54; pkt[o++] = 4;
        net_wr_be32(pkt + o, dhcp_server); o += 4;
    }
    pkt[o++] = 55; pkt[o++] = 3; pkt[o++] = 1; pkt[o++] = 3; pkt[o++] = 6;
    pkt[o++] = 255;

    u8 udp[340];
    net_wr_be16(udp, 68);
    net_wr_be16(udp + 2, 67);
    net_wr_be16(udp + 4, (u16)(8 + o));
    net_wr_be16(udp + 6, 0);
    memcpy(udp + 8, pkt, o);
    return ip_send_ex(0, 0xFFFFFFFFu, 17, udp, 8 + o);
}

int net_dhcp(void)
{
    if (!net_init())
        return -1;
    dhcp_xid = 0x47540000u | (timer_ticks() & 0xFFFF);
    dhcp_offer_type = 0;
    dhcp_yiaddr = dhcp_mask = dhcp_gw = dhcp_dns = dhcp_server = 0;
    dhcp_active = true;

    for (int retry = 0; retry < 3 && dhcp_offer_type != 2; retry++) {
        dhcp_send(1, 0);                   /* DISCOVER */
        u32 t0 = timer_uptime_ms();
        while (timer_uptime_ms() - t0 < 800 && dhcp_offer_type != 2) {
            net_poll();
            cpu_halt();
        }
    }
    if (dhcp_offer_type != 2) {
        dhcp_active = false;
        return -1;
    }
    dhcp_offer_type = 0;
    for (int retry = 0; retry < 3 && dhcp_offer_type != 5; retry++) {
        dhcp_send(3, dhcp_yiaddr);         /* REQUEST */
        u32 t0 = timer_uptime_ms();
        while (timer_uptime_ms() - t0 < 800 && dhcp_offer_type != 5) {
            net_poll();
            cpu_halt();
        }
    }
    dhcp_active = false;
    if (dhcp_offer_type != 5 || dhcp_yiaddr == 0)
        return -1;

    ip_addr = dhcp_yiaddr;
    ip_mask = dhcp_mask ? dhcp_mask : IP4(255, 255, 255, 0);
    ip_gw = dhcp_gw ? dhcp_gw : IP4(10, 0, 2, 2);
    ip_dns = dhcp_dns ? dhcp_dns : IP4(10, 0, 2, 3);
    return 0;
}

/* ----------------------------------------------------------- 收包入口 */
static void ip_input(const u8 *p, u32 n)
{
    if (n < 20 || (p[0] >> 4) != 4)
        return;
    u32 ihl = (u32)(p[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > n)
        return;
    u32 total = net_be16(p + 2);
    if (total < ihl || total > n)
        total = n;
    if (net_csum(p, ihl, 0) != 0)
        return;
    u16 frag = net_be16(p + 6);
    if (frag & 0x3FFF)                     /* 不分片重组 */
        return;

    u32 src = net_be32(p + 12);
    u32 dst = net_be32(p + 16);
    u8 proto = p[9];
    const u8 *pl = p + ihl;
    u32 plen = total - ihl;

    if (proto == 1) {
        if (dst == ip_addr)
            icmp_input(src, pl, plen);
    } else if (proto == 17) {
        udp_input(src, pl, plen);
    } else if (proto == 6) {
        if (dst == ip_addr)
            tcp_input(src, dst, pl, plen);
    }
}

static void net_handle_frame(const u8 *f, u32 n)
{
    if (n < 14)
        return;
    u16 type = net_be16(f + 12);
    if (type == 0x0806)
        arp_input(f + 14, n - 14);
    else if (type == 0x0800)
        ip_input(f + 14, n - 14);
}
