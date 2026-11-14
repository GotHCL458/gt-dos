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
        /* 程序执行模型 (P5) 接入后改为回跳到 shell; 当前无程序上下文 */
        r->rax = (u64)-1;
        break;
    default:
        r->rax = (u64)-1;
        break;
    }
}
