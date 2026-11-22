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
#define GT_SYS_CLS      38
#define GT_SYS_COLOR    39
#define GT_SYS_SCREEN   40
#define GT_SYS_UNLINK   41
#define GT_SYS_MKDIR    42
#define GT_SYS_RMDIR    43
#define GT_SYS_GETCWD   44
#define GT_SYS_HOSTNAME 45
#define GT_SYS_USERNAME 46
#define GT_SYS_ISADMIN  47
#define GT_SYS_VERSION  48
#define GT_SYS_UNIXTIME 49
#define GT_SYS_RICH     50

/* GUI 画布 / 绘图 (GTAPI, 需先 gui_begin) */
#define GT_SYS_GFX_INFO  64
#define GT_SYS_GUI_BEGIN 65
#define GT_SYS_GUI_END   66
#define GT_SYS_GFX_FILL  67
#define GT_SYS_GFX_FRAME 68
#define GT_SYS_GFX_TEXT  69
#define GT_SYS_GFX_CLEAR 70
#define GT_SYS_GUI_FLUSH 71
#define GT_SYS_GUI_POLL  72
#define GT_SYS_GFX_BAR   73

struct gt_gui_info { long w, h, scale, cw, ch; };
struct gt_gui_event { long x, y, buttons, key; };

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

/* 6 参数版本: 第 4/5/6 个参数走 rcx/r8/r9 (TC 代码生成与内核分派一致) */
static inline long gt_syscall6(long nr, long a, long b, long c,
                               long d, long e, long f)
{
    register long r8v __asm__("r8") = e;
    register long r9v __asm__("r9") = f;
    long ret;
    __asm__ __volatile__("int $0x21" : "=a"(ret)
                         : "a"(nr), "D"(a), "S"(b), "d"(c), "c"(d),
                           "r"(r8v), "r"(r9v)
                         : "r11", "memory");
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

/* 控制台 */
typedef struct { unsigned long cols, rows; } gt_screen_t;
static inline void gt_cls(void)
{ gt_syscall(GT_SYS_CLS, 0, 0, 0); }
static inline void gt_color(int fg, int bg)
{ gt_syscall(GT_SYS_COLOR, fg, bg, 0); }
static inline int gt_screen(gt_screen_t *sc)
{ return (int)gt_syscall(GT_SYS_SCREEN, (long)sc, 0, 0); }

/* 文件系统 */
static inline int gt_unlink(const char *path)
{ return (int)gt_syscall(GT_SYS_UNLINK, (long)path, 0, 0); }
static inline int gt_mkdir(const char *path)
{ return (int)gt_syscall(GT_SYS_MKDIR, (long)path, 0, 0); }
static inline int gt_rmdir(const char *path)
{ return (int)gt_syscall(GT_SYS_RMDIR, (long)path, 0, 0); }

/* 系统信息 */
static inline int gt_getcwd(char *buf, unsigned cap)
{ return (int)gt_syscall(GT_SYS_GETCWD, (long)buf, cap, 0); }
static inline int gt_hostname(char *buf, unsigned cap)
{ return (int)gt_syscall(GT_SYS_HOSTNAME, (long)buf, cap, 0); }
static inline int gt_username(char *buf, unsigned cap)
{ return (int)gt_syscall(GT_SYS_USERNAME, (long)buf, cap, 0); }
static inline int gt_isadmin(void)
{ return (int)gt_syscall(GT_SYS_ISADMIN, 0, 0, 0); }
static inline int gt_version(char *buf, unsigned cap)
{ return (int)gt_syscall(GT_SYS_VERSION, (long)buf, cap, 0); }
static inline long gt_unixtime(void)
{ return gt_syscall(GT_SYS_UNIXTIME, 0, 0, 0); }

/* 富文本 */
static inline int gt_rich(const char *md, unsigned len, const char *title)
{ return (int)gt_syscall(GT_SYS_RICH, (long)md, len, (long)title); }

/* ---------------------------------------------------------------- GUI 画布 */
static inline int gt_gfx_info(struct gt_gui_info *gi)
{ return (int)gt_syscall(GT_SYS_GFX_INFO, (long)gi, 0, 0); }
static inline int gt_gui_begin(void)
{ return (int)gt_syscall(GT_SYS_GUI_BEGIN, 0, 0, 0); }
static inline void gt_gui_end(void)
{ gt_syscall(GT_SYS_GUI_END, 0, 0, 0); }
static inline void gt_gfx_clear(unsigned color)
{ gt_syscall(GT_SYS_GFX_CLEAR, (long)color, 0, 0); }
static inline void gt_gfx_fill(int x, int y, int w, int h, unsigned color)
{ gt_syscall6(GT_SYS_GFX_FILL, x, y, w, h, color, 0); }
static inline void gt_gfx_frame(int x, int y, int w, int h, unsigned color)
{ gt_syscall6(GT_SYS_GFX_FRAME, x, y, w, h, color, 0); }
static inline int gt_gfx_text(int x, int y, const char *s, unsigned color)
{ return (int)gt_syscall6(GT_SYS_GFX_TEXT, x, y, (long)s, color, 0, 0); }
static inline void gt_gfx_bar(int x, int y, int w, int h, int pct,
                              unsigned color)
{ gt_syscall6(GT_SYS_GFX_BAR, x, y, w, h, pct, color); }
static inline void gt_gui_flush(void)
{ gt_syscall(GT_SYS_GUI_FLUSH, 0, 0, 0); }
static inline int gt_gui_poll(struct gt_gui_event *ev)
{ return (int)gt_syscall(GT_SYS_GUI_POLL, (long)ev, 0, 0); }

#endif /* _GT_UNISTD_H */
