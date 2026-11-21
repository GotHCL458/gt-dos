/* INT 0x21 系统调用门: 按 rax 分派, 参数取 SysV 寄存器, 结果写回 rax.
   程序运行于 ring0, 本门是 API 入口而非权限边界; reboot/shutdown 需管理员. */
#include "gt.h"

/* ------------------------------------------- SYS_PRINTF: 小型格式化输出
   rdi=fmt, rsi=可变参数个数, rdx=参数数组 (指向最后一个参数).
   参数由 TC 在调用点压栈, 顺序为 args[0]=最后一个参数. 支持
   %d %i %u %x %X %c %s %p %%; 宽度/精度修饰被忽略. */
static void fmt_u64(u64 v, u32 base, bool upper)
{
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *dig = upper ? up : lo;
    char buf[24];
    int n = 0;
    if (v == 0) {
        console_putc('0');
        return;
    }
    while (v) {
        buf[n++] = dig[v % base];
        v /= base;
    }
    while (n)
        console_putc(buf[--n]);
}

static void fmt_i64(i64 v)
{
    if (v < 0) {
        console_putc('-');
        fmt_u64((u64)(-(v + 1)) + 1u, 10, false);
    } else {
        fmt_u64((u64)v, 10, false);
    }
}

/* %f: 纯整数运算把 IEEE-754 位模式打印为 6 位小数 */
static void fmt_double(u64 bits)
{
    int sign = (int)(bits >> 63);
    int ef = (int)((bits >> 52) & 0x7FF);
    u64 m = bits & ((1ULL << 52) - 1);

    if (ef == 0x7FF) {
        if (sign) console_putc('-');
        console_puts("inf");
        return;
    }
    if (ef == 0)
        m = 0;                          /* 次正规/零按 0 处理 */
    else
        m |= 1ULL << 52;
    int e2 = (ef == 0) ? -1074 : ef - 1023 - 52;

    u64 ip = 0, frac6 = 0;
    if (m != 0 && e2 >= 0) {
        ip = (e2 > 11) ? 0xFFFFFFFFFFFFFFFFULL : (m << e2);
    } else if (m != 0) {
        int sh = -e2;
        if (sh >= 64) {
            unsigned __int128 v = (unsigned __int128)m * 1000000u;
            if (sh < 127) {
                if (sh < 64)
                    v += (unsigned __int128)1 << (sh - 1);
                frac6 = (u64)(v >> sh);
            }
        } else {
            ip = m >> sh;
            u64 rem = m & ((1ULL << sh) - 1);
            unsigned __int128 v = (unsigned __int128)rem * 1000000u;
            if (sh > 0)
                v += (unsigned __int128)1 << (sh - 1);
            frac6 = (u64)(v >> sh);
            if (frac6 >= 1000000u) {
                frac6 -= 1000000u;
                ip++;
            }
        }
    }

    if (sign) console_putc('-');
    fmt_u64(ip, 10, false);
    console_putc('.');
    char b[6];
    for (int i = 5; i >= 0; i--) {
        b[i] = (char)('0' + (frac6 % 10u));
        frac6 /= 10u;
    }
    for (int i = 0; i < 6; i++)
        console_putc(b[i]);
}

static void fmt_printf(const char *fmt, int nargs, const u64 *args)
{
    int ai = nargs - 1;              /* args[nargs-1] 是第一个可变参数 */
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }
        p++;
        if (*p == '%') {
            console_putc('%');
            continue;
        }
        while (*p >= '0' && *p <= '9')
            p++;                     /* 跳过宽度 */
        if (*p == '.') {             /* 跳过精度 */
            p++;
            while (*p >= '0' && *p <= '9')
                p++;
        }
        while (*p == 'l' || *p == 'h' || *p == 'z')
            p++;                     /* 长度修饰: 统一按 64 位处理 */
        u64 v = (ai >= 0) ? args[ai--] : 0;
        switch (*p) {
        case 'd':
        case 'i': fmt_i64((i64)v); break;
        case 'f':
        case 'F': fmt_double(v); break;
        case 'u': fmt_u64(v, 10, false); break;
        case 'x': fmt_u64(v, 16, false); break;
        case 'X': fmt_u64(v, 16, true); break;
        case 'p':
            console_puts("0x");
            fmt_u64(v, 16, false);
            break;
        case 'c': console_putc((char)v); break;
        case 's':
            if (v) console_puts((const char *)v);
            else console_puts("(null)");
            break;
        case 0:
            return;
        default:
            console_putc('%');
            console_putc(*p);
            ai++;                    /* 未知格式不消耗参数 */
            break;
        }
    }
}

/* 把 C 字符串复制到用户缓冲 (始终 NUL 结尾), 返回长度或 -1 */
static int out_str(const char *src, void *dst, u64 cap)
{
    if (!dst || cap == 0)
        return -1;
    if (cap > 0x100000)
        cap = 0x100000;
    char *d = (char *)dst;
    u32 n = 0;
    if (src) {
        while (src[n] && n + 1 < (u32)cap) {
            d[n] = src[n];
            n++;
        }
    }
    d[n] = 0;
    return (int)n;
}

/* 公历日期 -> 1970 起的天数 */
static i64 days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (i64)era * 146097 + doe - 719468;
}

void syscall_handle(struct regs *r)
{
    u64 nr = r->rax;
    switch (nr) {
    case SYS_PUTC:
        console_putc((char)r->rdi);
        r->rax = 0;
        break;
    case SYS_PUTS:
        if (r->rdi) console_puts((const char *)r->rdi);
        r->rax = 0;
        break;
    case SYS_GETC:
        r->rax = (u64)ui_read_key();
        break;
    case SYS_WRITE: {
        int fd = (int)r->rdi;
        const void *buf = (const void *)r->rsi;
        u32 n = (u32)r->rdx;
        if (!buf) { r->rax = (u64)-1; break; }
        if (fd == 1 || fd == 2) { console_write((const char *)buf, n); r->rax = n; }
        else if (fd == 0) { r->rax = (u64)-1; }
        else { r->rax = (u64)fat_fd_write(fd, buf, n); }
        break;
    }
    case SYS_READ: {
        int fd = (int)r->rdi;
        void *buf = (void *)r->rsi;
        u32 n = (u32)r->rdx;
        if (!buf) { r->rax = (u64)-1; break; }
        if (fd == 0) { r->rax = (u64)ui_read_key(); }
        else { r->rax = (u64)fat_fd_read(fd, buf, n); }
        break;
    }
    case SYS_OPEN:
        r->rax = (u64)fat_fd_open((const char *)r->rdi, (int)r->rsi);
        break;
    case SYS_CLOSE:
        r->rax = (u64)fat_fd_close((int)r->rdi);
        break;
    case SYS_SEEK:
        r->rax = (u64)fat_fd_seek((int)r->rdi, (int)r->rsi, (int)r->rdx);
        break;
    case SYS_STAT:
        r->rax = (u64)fat_stat_pub((const char *)r->rdi, (struct gt_stat *)r->rsi);
        break;
    case SYS_STATX:
        r->rax = (u64)fat_statx((const char *)r->rdi, (struct gt_statx *)r->rsi);
        break;
    case SYS_IPRINT:
        kprintf("%d", (int)r->rdi);
        r->rax = 0;
        break;
    case SYS_NETINFO:
        if (!r->rdi) { r->rax = (u64)-1; break; }
        net_init();
        net_get_info((struct net_info *)r->rdi);
        r->rax = 0;
        break;
    case SYS_TCP_CONNECT:
        r->rax = (u64)tcp_connect((u32)r->rdi, (u16)r->rsi);
        break;
    case SYS_TCP_SEND:
        r->rax = (u64)tcp_send((int)r->rdi, (const void *)r->rsi, (u32)r->rdx);
        break;
    case SYS_TCP_RECV:
        r->rax = (u64)tcp_recv((int)r->rdi, (void *)r->rsi, (u32)r->rdx);
        break;
    case SYS_TCP_CLOSE:
        r->rax = (u64)tcp_close((int)r->rdi);
        break;
    case SYS_TCP_LISTEN:
        r->rax = (u64)tcp_listen((u16)r->rdi);
        break;
    case SYS_TCP_ACCEPT:
        r->rax = (u64)tcp_accept((int)r->rdi);
        break;
    case SYS_DHCP:
        cpu_sti();
        r->rax = (u64)net_dhcp();
        cpu_cli();
        break;
    case SYS_DNS: {
        u32 ip = 0;
        r->rax = net_dns_resolve((const char *)r->rdi, &ip) == 0 ? (u64)ip : 0;
        break;
    }
    case SYS_TFTP_GET:
        r->rax = (u64)net_tftp_fetch((u32)r->rdi, 69,
                                     (const char *)r->rsi, (const char *)r->rdx);
        break;
    case SYS_HTTP_GET:
        r->rax = (u64)net_http_fetch((const char *)r->rdi,
                                     (const char *)r->rsi, (const char *)r->rdx);
        break;
    case SYS_UDP_OPEN:
        r->rax = (u64)udp_open((u16)r->rdi);
        break;
    case SYS_UDP_SENDTO:
        /* rdi=fd, rsi=ip, rdx=port, rcx=data, r8=len */
        r->rax = (u64)udp_sendto((int)r->rdi, (u32)r->rsi, (u16)r->rdx,
                                 (const void *)r->rcx, (u32)r->r8);
        break;
    case SYS_UDP_RECVFROM: {
        /* rdi=fd, rsi=buf, rdx=cap, rcx=timeout_ms, r8=src*, r9=sport* */
        u32 src_ip = 0;
        u16 src_port = 0;
        int n = udp_recvfrom((int)r->rdi, (void *)r->rsi, (u32)r->rdx,
                             (u32)r->rcx, &src_ip, &src_port);
        if (r->r8)
            *(u32 *)r->r8 = src_ip;
        if (r->r9)
            *(u16 *)r->r9 = src_port;
        r->rax = (u64)n;
        break;
    }
    case SYS_ICMP_ERR:
        r->rax = (u64)net_icmp_last_error((struct gt_icmp_err *)r->rdi);
        break;
    case SYS_UDP_CLOSE:
        udp_close((int)r->rdi);
        r->rax = 0;
        break;
    case SYS_OPENDIR:
        r->rax = (u64)fat_fd_opendir((const char *)r->rdi);
        break;
    case SYS_READDIR: {
        struct fat_dirent d;
        int rc = fat_fd_readdir((int)r->rdi, &d);
        if (rc == 1 && r->rsi) {
            struct gt_dirent *o = (struct gt_dirent *)r->rsi;
            int i = 0;
            while (i < 15 && d.name[i]) { o->name[i] = d.name[i]; i++; }
            o->name[i] = '\0';
            o->size = d.size;
            o->attr = d.attrs;
            o->is_dir = d.is_dir ? 1 : 0;
        }
        r->rax = (u64)rc;
        break;
    }
    case SYS_SLEEP:
        thread_sleep_ms((u32)r->rdi);   /* 只阻塞当前线程 */
        r->rax = 0;
        break;
    case SYS_LIBLOAD:
        r->rax = (u64)lib_load((const char *)r->rdi);
        break;
    case SYS_LIBSYM:
        r->rax = lib_sym((int)r->rdi, (const char *)r->rsi);
        break;
    case SYS_UPTIME:
        r->rax = (u64)timer_uptime_ms();
        break;
    case SYS_GETTIME: {
        struct gt_time *t = (struct gt_time *)r->rdi;
        if (!t) { r->rax = (u64)-1; break; }
        rtc_read(&t->year, &t->month, &t->day, &t->hour, &t->min, &t->sec);
        r->rax = 0;
        break;
    }
    case SYS_REBOOT:
        if (user_is_admin()) power_reboot();
        r->rax = (u64)-1;
        break;
    case SYS_SHUTDOWN:
        if (user_is_admin()) power_shutdown();
        r->rax = (u64)-1;
        break;
    case SYS_PRINTF: {
        int n = (int)r->rsi;
        if (n < 0) n = 0;
        if (n > 8) n = 8;
        if (r->rdi)
            fmt_printf((const char *)r->rdi, n, (const u64 *)r->rdx);
        r->rax = 0;
        break;
    }
    case SYS_CLS:
        console_clear();
        r->rax = 0;
        break;
    case SYS_COLOR:
        gt_color2((u8)(r->rdi & 0xF), (u8)(r->rsi & 0xF));
        r->rax = 0;
        break;
    case SYS_SCREEN: {
        struct gt_screen *o = (struct gt_screen *)r->rdi;
        if (!o) { r->rax = (u64)-1; break; }
        u64 cols = VGA_COLS, rows = VGA_ROWS;
        if (gfx_active() && gfx_cell_w() > 0 && gfx_cell_h() > 0) {
            cols = (u64)(gfx_w() / gfx_cell_w());
            rows = (u64)(gfx_h() / gfx_cell_h());
        }
        o->cols = cols;
        o->rows = rows;
        r->rax = 0;
        break;
    }
    case SYS_UNLINK:
        if (!r->rdi) { r->rax = (u64)-1; break; }
        r->rax = fat_delete((const char *)r->rdi) ? 0 : (u64)-1;
        break;
    case SYS_MKDIR:
        if (!r->rdi) { r->rax = (u64)-1; break; }
        r->rax = fat_mkdir((const char *)r->rdi) ? 0 : (u64)-1;
        break;
    case SYS_RMDIR:
        if (!r->rdi) { r->rax = (u64)-1; break; }
        r->rax = fat_rmdir((const char *)r->rdi) ? 0 : (u64)-1;
        break;
    case SYS_GETCWD: {
        const char *c = shell_cwd();
        r->rax = (u64)out_str((c && c[0]) ? c : "\\", (void *)r->rdi, r->rsi);
        break;
    }
    case SYS_HOSTNAME:
        r->rax = (u64)out_str(gcfg.hostname, (void *)r->rdi, r->rsi);
        break;
    case SYS_USERNAME:
        r->rax = (u64)out_str(user_name(), (void *)r->rdi, r->rsi);
        break;
    case SYS_ISADMIN:
        r->rax = user_is_admin() ? 1 : 0;
        break;
    case SYS_VERSION:
        r->rax = (u64)out_str(GT_VERSION, (void *)r->rdi, r->rsi);
        break;
    case SYS_UNIXTIME: {
        int yy, mm, dd, hh, mi, ss;
        rtc_read(&yy, &mm, &dd, &hh, &mi, &ss);
        if (yy < 1970 || mm < 1 || mm > 12 || dd < 1 || dd > 31) {
            r->rax = (u64)-1;
            break;
        }
        i64 secs = days_from_civil(yy, mm, dd) * 86400 +
                   (i64)hh * 3600 + (i64)mi * 60 + ss;
        r->rax = (u64)secs;
        break;
    }
    case SYS_RICH: {
        u32 len = (u32)r->rsi;
        if (!r->rdi) { r->rax = (u64)-1; break; }
        if (len > 0x10000) len = 0x10000;
        rich_render((const char *)r->rdi, len, (const char *)r->rdx);
        r->rax = 0;
        break;
    }
    case SYS_THREAD_CREATE:
        r->rax = (u64)thread_create((thread_fn_t)r->rdi, (void *)r->rsi);
        break;
    case SYS_YIELD:
        thread_yield();
        r->rax = 0;
        break;
    case SYS_THREAD_EXIT:
        thread_exit_current((int)r->rdi);   /* 不返回 */
        break;
    case SYS_THREAD_JOIN:
        r->rax = (u64)thread_join((int)r->rdi);
        break;
    case SYS_GETTID:
        r->rax = (u64)sched_current_id();
        break;
    case SYS_MUTEX_CREATE:
        r->rax = (u64)mutex_create();
        break;
    case SYS_MUTEX_LOCK:
        r->rax = (u64)mutex_lock((int)r->rdi);
        break;
    case SYS_MUTEX_UNLOCK:
        r->rax = (u64)mutex_unlock((int)r->rdi);
        break;
    case SYS_THREAD_DETACH:
        r->rax = (u64)thread_detach((int)r->rdi);
        break;
    case SYS_CRITICAL_ENTER:
        sched_syscall_enter();              /* 嵌套计数: 定时器暂停抢占 */
        r->rax = 0;
        break;
    case SYS_CRITICAL_EXIT:
        sched_syscall_exit();
        r->rax = 0;
        break;
    case SYS_MUTEX_TRYLOCK:
        r->rax = (u64)mutex_trylock((int)r->rdi);
        break;
    case SYS_MUTEX_DESTROY:
        r->rax = (u64)mutex_destroy((int)r->rdi);
        break;
    case SYS_EXIT:
        if (!exec_is_active()) { r->rax = (u64)-1; break; }
        if (!sched_current_is_main()) {
            thread_exit_current((int)r->rdi);   /* 子线程退出, 不结束程序 */
            break;
        }
        sched_syscall_exit();                   /* exec_exit 不返回, 先释放调度锁 */
        exec_exit((int)r->rdi);
        break;
    default:
        r->rax = (u64)-1;
        break;
    }
}
