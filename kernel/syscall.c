/* INT 0x21 系统调用门: 按 rax 分派, 参数取 SysV 寄存器, 结果写回 rax.
   程序运行于 ring0, 本门是 API 入口而非权限边界; reboot/shutdown 需管理员. */
#include "gt.h"

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
        /* 中断门进入时 IF=0, 而 timer_sleep_ms 依赖 hlt 被时钟中断唤醒 */
        cpu_sti();
        timer_sleep_ms((u32)r->rdi);
        cpu_cli();
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
    case SYS_EXIT:
        if (exec_is_active()) exec_exit((int)r->rdi);
        r->rax = (u64)-1;
        break;
    default:
        r->rax = (u64)-1;
        break;
    }
}
