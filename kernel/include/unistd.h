/*
 * GT-DOS 系统调用包装 (INT 0x21)
 * ABI: rax=调用号, rdi/rsi/rdx 为参数, 返回值在 rax.
 */
#ifndef _GT_UNISTD_H
#define _GT_UNISTD_H

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200

#define GT_SYS_EXIT     0
#define GT_SYS_PUTC     1
#define GT_SYS_GETC     2
#define GT_SYS_WRITE    3
#define GT_SYS_READ     4
#define GT_SYS_OPEN     5
#define GT_SYS_CLOSE    6
#define GT_SYS_UPTIME   7
#define GT_SYS_REBOOT   8
#define GT_SYS_SHUTDOWN 9
#define GT_SYS_GETTIME  10
#define GT_SYS_PUTS     11
#define GT_SYS_SEEK     12
#define GT_SYS_STAT     13
#define GT_SYS_OPENDIR  14
#define GT_SYS_READDIR  15
#define GT_SYS_SLEEP    16
#define GT_SYS_LIBLOAD  17
#define GT_SYS_LIBSYM   18
#define GT_SYS_STATX    19
#define GT_SYS_IPRINT   20

static inline long gt_syscall3(long nr, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__("int $0x21" : "=a"(ret)
                         : "a"(nr), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return ret;
}

static inline long gt_syscall(long nr, long a, long b, long c)
{
    return gt_syscall3(nr, a, b, c);
}

static inline int gt_write(int fd, const void *buf, unsigned n)
{ return (int)gt_syscall(GT_SYS_WRITE, fd, (long)buf, n); }
static inline int gt_read(int fd, void *buf, unsigned n)
{ return (int)gt_syscall(GT_SYS_READ, fd, (long)buf, n); }
static inline int gt_open(const char *path, int flags)
{ return (int)gt_syscall(GT_SYS_OPEN, (long)path, flags, 0); }
static inline int gt_close(int fd)
{ return (int)gt_syscall(GT_SYS_CLOSE, fd, 0, 0); }
static inline int gt_seek(int fd, int offset, int whence)
{ return (int)gt_syscall(GT_SYS_SEEK, fd, offset, whence); }
static inline unsigned gt_uptime(void)
{ return (unsigned)gt_syscall(GT_SYS_UPTIME, 0, 0, 0); }
static inline void gt_sleep(unsigned ms)
{ gt_syscall(GT_SYS_SLEEP, ms, 0, 0); }
static inline void gt_putc(int c)
{ gt_syscall(GT_SYS_PUTC, c, 0, 0); }
static inline int gt_getc(void)
{ return (int)gt_syscall(GT_SYS_GETC, 0, 0, 0); }
static inline void gt_exit(int code)
{ gt_syscall(GT_SYS_EXIT, code, 0, 0); for (;;) ; }

#endif /* _GT_UNISTD_H */
