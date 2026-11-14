/* GT-DOS 文件/控制台系统调用包装 (INT 0x21) */
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

#define GT_SYS_EXIT  0
#define GT_SYS_PUTC  1
#define GT_SYS_GETC  2
#define GT_SYS_WRITE 3
#define GT_SYS_READ  4
#define GT_SYS_OPEN  5
#define GT_SYS_CLOSE 6

static inline long gt_syscall(long nr, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__("int $0x21" : "=a"(ret)
                         : "a"(nr), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return ret;
}
static inline int gt_write(int fd, const void *buf, unsigned n)
{ return (int)gt_syscall(GT_SYS_WRITE, fd, (long)buf, n); }
static inline int gt_read(int fd, void *buf, unsigned n)
{ return (int)gt_syscall(GT_SYS_READ, fd, (long)buf, n); }
static inline int gt_open(const char *path, int flags)
{ return (int)gt_syscall(GT_SYS_OPEN, (long)path, flags, 0); }
static inline int gt_close(int fd)
{ return (int)gt_syscall(GT_SYS_CLOSE, fd, 0, 0); }
static inline void gt_putc(int c)
{ gt_syscall(GT_SYS_PUTC, c, 0, 0); }
static inline int gt_getc(void)
{ return (int)gt_syscall(GT_SYS_GETC, 0, 0, 0); }
static inline void gt_exit(int code)
{ gt_syscall(GT_SYS_EXIT, code, 0, 0); for (;;) ; }
#endif /* _GT_UNISTD_H */