/* ==========================================================================
 *  GT-DOS 内核公共头文件
 * ========================================================================== */
#ifndef GT_H
#define GT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long long          i64;

/* ---------------------------------------------------------------- 端口 I/O */
static inline void outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" ::"a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 r;
    __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void outw(u16 port, u16 val)
{
    __asm__ __volatile__("outw %0, %1" ::"a"(val), "Nd"(port));
}

static inline u16 inw(u16 port)
{
    u16 r;
    __asm__ __volatile__("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void outl(u16 port, u32 val)
{
    __asm__ __volatile__("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline u32 inl(u16 port)
{
    u32 r;
    __asm__ __volatile__("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void io_wait(void) { outb(0x80, 0); }

static inline void cpu_halt(void) { __asm__ __volatile__("hlt"); }
static inline void cpu_cli(void)  { __asm__ __volatile__("cli"); }
static inline void cpu_sti(void)  { __asm__ __volatile__("sti"); }

void softirq_dispatch(void);           /* 前置声明 (cpu_relax 使用) */
void kernel_idle_check(void);          /* 空闲期内核完整性复核 (protect.c) */

/* 空闲等待: hlt 唤醒后顺便派发软中断 (bottom-half 在非中断上下文执行) */
static inline void cpu_relax(void)
{
    cpu_halt();
    softirq_dispatch();
    kernel_idle_check();
}

u64 gt_read_cr2(void);

/* ------------------------------------------------------------------ 串口 */
#define COM1_BASE 0x3F8
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_write(const char *s, size_t n);
bool serial_has_data(void);
int  serial_getchar(void);     /* 无数据返回 -1 */

/* -------------------------------------------------------------------- VGA */
#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_MEM  ((volatile u16 *)0xB8000)
#define HIST_LINES 1024            /* 回滚缓冲行数 */

/* 引导器邮箱 (boot.asm 写入) */
#define MB_BOOT_DRIVE 0x0600       /* u8  引导盘号 */
#define MB_FONT_LBA   0x0604       /* u32 font.gtd 起始 LBA */
#define MB_FONT_SECT  0x0608       /* u32 font.gtd 扇区数 */
#define MB_CPM_LBA    0x060C       /* u32 cpm.gtd 起始 LBA */
#define MB_CPM_SECT   0x0610       /* u32 cpm.gtd 扇区数 */

/* 16 色 VGA 调色板 */
enum {
    C_BLACK = 0, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN,
    C_LGRAY, C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA,
    C_YELLOW, C_WHITE
};

void vga_init(void);
void vga_load_font(void);            /* 保护模式下用 ATA 读取汉字字库 */
void vga_clear(void);
void vga_putc(char c);
void vga_putc_cp(u32 cp);             /* 写一个 Unicode 码点 (UTF-8 解码用) */
void vga_set_color(u8 fg, u8 bg);
u8   vga_get_fg(void);
u8   vga_get_bg(void);
void vga_get_cursor(int *row, int *col);
void vga_set_col(int col);            /* 移动当前行光标列 */
void vga_line_reset(void);            /* 清空当前行, 光标归零 */
void vga_write_raw(const char *s, int n); /* 直接写入当前行, UTF-8 解码 */
void vga_line_sync(void);                 /* 行编辑收尾: 只重绘变化的单元 */
int  vga_utf8_cols(const char *s);        /* UTF-8 字符串显示列数 */
void vga_erase_eol(void);             /* 清除光标到行尾 */
void vga_set_cursor_visible(bool on);
void vga_puts(const char *s);          /* 仅 VGA 输出 (TUI 装饰) */
void vga_set_status(const char *left, const char *right); /* 底部状态栏 */
void vga_clear_status(void);
void vga_scroll_view(int delta);      /* 回滚视图: 正=向上看历史 */
int  vga_view_offset(void);
int  vga_view_max(void);              /* 可回滚的最大行数 (上限) */
void vga_scroll_reset(void);          /* 回到最新输出 */
void vga_set_scroll_msg(const char *s); /* 状态栏滚动提示 */
void vga_set_attr_default(u8 a);
const u8 *vga_glyph(u32 cp);         /* 取码点 32 字节字模 (GUI 用, 可返回 0) */

/* ------------------------------------------------------- 共享图形层 (gfx) */
bool gfx_available(void);            /* Bochs VBE 可用? */
bool gfx_init(void);                 /* 分辨率自适配 (优先 1920x1080) + 双缓冲 */
void gfx_shutdown(void);
bool gfx_active(void);
int  gfx_w(void);                    /* 当前分辨率宽 */
int  gfx_h(void);                    /* 当前分辨率高 */
int  gfx_scale(void);                /* 字模整数倍缩放 (1..3) */
int  gfx_cell_w(void);               /* 文本单元宽 (8*scale) */
int  gfx_cell_h(void);               /* 文本单元高 (16*scale) */
void gfx_clear(u32 c);
void gfx_fill(int x, int y, int w, int h, u32 c);
void gfx_frame(int x, int y, int w, int h, u32 c);
void gfx_bevel(int x, int y, int w, int h, u32 light, u32 dark);
int  gfx_char(int x, int y, u32 cp, u32 fg);   /* 返回前进像素宽 */
int  gfx_text(int x, int y, const char *s, u32 fg);
int  gfx_text_w(const char *s);
void gfx_flip(void);                 /* 整屏提交后台缓冲 */
void gfx_flip_rect(int x, int y, int w, int h);

/* ---------------------------------------------------------------- GUI */
void vga_enter_gfx(void);            /* 控制台切到图形文本后端 */
void vga_refresh(void);              /* 重绘整个文本控制台 */

/* ---------------------------------------------------------------- TUI */
void tui_splash(void);                /* 启动画面 (含短暂停留) */
void tui_frame_open(const char *title); /* 双线框标题 (VGA) + 纯文本 (串口) */
void tui_frame_close(void);
void tui_puts(const char *s);         /* 同时写 VGA(带色) 与串口(纯文本) */
void tui_clock_update(void);          /* 刷新状态栏时钟 (定时器钩子) */
void tui_status_refresh(void);        /* 重绘状态栏 (shell 调用) */

/* ------------------------------------------------------- 国际化 (i18n) */
/* 语言: 0=English, 1=中文 */
void      i18n_set_lang(int lang);
int       i18n_lang(void);
const char *L(const char *key);        /* 取当前语言的文案 (缺省返回 key) */

/* ---------------------------------------------------------------- 磁盘 */
/* ATA PIO primary 通道: dev0=master(-hda), dev1=slave(-hdb) */
bool ata_init(void);
bool ata_present(void);              /* dev0 存在 (兼容) */
u32  ata_sectors(void);
bool ata_read(u32 lba, u32 count, void *buf);
bool ata_write(u32 lba, u32 count, const void *buf);
const char *ata_model_string(void);
int  ata_ndev(void);
bool ata_present_dev(int dev);
u32  ata_sectors_dev(int dev);
const char *ata_model_dev(int dev);
bool ata_read_dev(int dev, u32 lba, u32 count, void *buf);
bool ata_write_dev(int dev, u32 lba, u32 count, const void *buf);

/* ------------------------------------------------------- FAT 文件系统 */
#define FAT_NAME_LEN 16             /* 8.3 展开为 "name.ext" 含结束符 */

/* DOS 文件属性位 */
#define ATTR_RO  0x01               /* 只读 */
#define ATTR_HID 0x02               /* 隐藏 */
#define ATTR_SYS 0x04               /* 系统 */
#define ATTR_VOL 0x08               /* 卷标 */
#define ATTR_DIR 0x10               /* 目录 */
#define ATTR_ARC 0x20               /* 归档 */

struct fat_dirent {
    char     name[FAT_NAME_LEN];    /* 已转为小写含点 */
    u32      size;
    u16      first_cluster;
    u8       attrs;
    bool     is_dir;
};

bool     fat_mount(void);            /* 挂载全部 ATA 卷 */
bool     fat_ready(void);
bool     fat_ready_drive(char letter);
bool     fat_set_drive(char letter);
char     fat_drive(void);
int      fat_drive_count(void);
char     fat_drive_letter(int i);
u32      fat_total_clusters(void);
u32      fat_free_clusters(void);
bool     fat_find(const char *name, struct fat_dirent *out);
bool     fat_read_file(const char *name, void *buf, u32 max, u32 *out_len);
bool     fat_create(const char *name, const void *data, u32 len);
bool     fat_delete(const char *name);
bool     fat_mkdir(const char *name);
bool     fat_rmdir(const char *name);
bool     fat_setattr(const char *name, u8 attrs);
bool     fat_getattr(const char *name, u8 *attrs);
bool     fat_touch(const char *name);
int      fat_list_dir(const char *path, struct fat_dirent *out, int max);
void     fat_set_show_all(bool on);    /* DIR /A: 显示隐藏/系统条目 */
void     fat_set_allow_sys(bool on);   /* 系统内部写 \GT-DOS\ 状态文件 */
bool     fat_read_dir_file(const char *dir, const char *name, void *buf,
                           u32 max, u32 *out_len);
u32      gt_encrypt(const u8 *in, u32 len, u8 *out);  /* AES-128-CBC, 返回密文长 */
u32      gt_decrypt(const u8 *in, u32 clen, u8 *out); /* 返回明文长, 失败 0 */
u32      gt_crc32(const void *data, u32 len);         /* IEEE CRC32 */
bool     fat_path_protected(const char *path);/* \GT-DOS\ 下的路径 */

/* 文件描述符层 (供 INT 0x21 系统调用): fd 0/1/2 = 控制台 */
#define FD_MAX 8
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x100
#define O_TRUNC  0x200
int  fat_fd_open(const char *path, int flags);
int  fat_fd_read(int fd, void *buf, u32 n);
int  fat_fd_write(int fd, const void *buf, u32 n);
int  fat_fd_close(int fd);
int  fat_fd_seek(int fd, int offset, int whence);
int  fat_fd_opendir(const char *path);
int  fat_fd_readdir(int fd, struct fat_dirent *out);

/* 用户程序可见的元数据 (与 stat.h / dirent.h 保持一致) */
struct gt_stat   { u32 size; u32 attr; u32 is_dir; };
struct gt_dirent { char name[16]; u32 size; u32 attr; u32 is_dir; };
int  fat_stat_pub(const char *path, struct gt_stat *out);

/* ------------------------------------------- exFAT 厂商扩展目录项 (0xE0) */
#define GT_XTYPE_ENCRYPT 1           /* 加密元数据 */
#define GT_XTYPE_ACL     2           /* 所有者与权限 */
#define GT_XTYPE_VERSION 3           /* 版本与 CRC32 校验 */

#define GT_XFLAG_ENCRYPTED 0x01      /* 文件内容为 GTE1 密文 */

#define GT_ACL_OWNER_WRITE 0x01
#define GT_ACL_OTHER_WRITE 0x02
#define GT_ACL_OTHER_READ  0x04

/* 扩展元数据 (SYS_STATX = 19) */
struct gt_statx {
    u32 size;
    u32 attr;
    u32 is_dir;
    u32 flags;                       /* GT_XFLAG_* */
    u32 version;
    u32 crc;                         /* 明文 CRC32 */
    char owner[16];
    u16 mode;                        /* GT_ACL_* */
    u16 reserved;
};
int  fat_statx(const char *path, struct gt_statx *out);

/* ------------------------------------- Rust 格式解析器 (rust/gtcore) */
/* .gtl 共享库: 头部校验 + 导出表 (C 侧只负责映射与跳转) */
struct gt_gtl_export {
    char name[24];
    u32  rva;
};
struct gt_gtl_info {
    u32 version;
    u32 image_size;
    u32 image_off;
    u32 count;
    struct gt_gtl_export exports[32];
};
int gt_gtl_parse(const u8 *buf, u32 len, struct gt_gtl_info *out);

/* .gtd 字模: 码点表二分查找, 返回 128 字节字模或 0 */
const u8 *gt_gtd_lookup(const u16 *cpm, u32 count, const u8 *font, u32 cp);

/* .gtp 程序: 明文 CRC32 完整性校验 (0=通过) */
int gt_gtp_verify(const u8 *buf, u32 len, u32 expect_crc);

/* exFAT 目录项集解析结果 (0x85 + 0xC0/0xC1/0xE0) */
struct gt_exfat_entry {
    u16 attr;
    u8  no_fat;
    u8  xflags;
    u32 count;                        /* 目录项集总项数 */
    u64 size;                         /* 磁盘上的数据长度 */
    u32 first;                        /* 起始簇 */
    u32 plain_size;
    u32 crc;                          /* 明文 CRC32 */
    u32 version;
    u16 mode;
    u16 nlen;
    u8  owner[16];
    u8  name[64];
};
int gt_exfat_parse(const u8 *ents, u32 avail_ents,
                   struct gt_exfat_entry *out);

/* SHA-256 (内核容器自检), 结果 32 字节 */
u32 gt_sha256(const u8 *data, u32 len, u8 out[32]);

/* -------------------------------------------- 内核容器/防篡改 (protect.c) */
int  kernel_self_check(void);                          /* 0=通过 */
int  kernel_reverify(void);                            /* 只读段 CRC32 复核 */
bool kernel_protected_info(u32 *image_len, u32 *crc, u8 digest[32]);

/* ------------------------------------------------- 内置 C 编译器 (TC) */
#define TC_SRC_MAX 32768
const u8  *tc_compile(const char *src, u32 len, u32 *out_len,
                      const char *path);
const char *tc_error(void);

/* ------------------------------------------------------- 富文本 / 编辑器 */
void rich_render(const char *text, u32 len, const char *title);
int  vim_run(const char *abs);          /* 0=正常, -1 无图形, -2 过大, -3 读失败 */

/* ------------------------------------------------------- 网络 (TCP/IP) */
#define IP4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | \
                         ((u32)(c) << 8) | (u32)(d))

struct net_info {
    u32 ip, mask, gw, dns;
    u8  mac[6];
    u8  link;
    u16 mtu;
};

bool net_init(void);
void net_poll(void);
bool net_link_up(void);
void net_get_info(struct net_info *out);
u32  net_ip(void);
u32  net_mask(void);
u32  net_gw(void);
u32  net_dns(void);
int  net_dhcp(void);
int  net_ping_one(u32 ip, u16 seq, u32 *rtt_ms);
bool net_parse_ip(const char *s, u32 *out);
void net_ip_str(u32 ip, char *buf);
u16  net_be16(const u8 *p);
u32  net_be32(const u8 *p);
void net_wr_be16(u8 *p, u16 v);
void net_wr_be32(u8 *p, u32 v);
u16  net_csum(const u8 *p, u32 len, u32 sum);
bool net_send_ip(u32 dst, u8 proto, const void *payload, u32 len);

/* UDP socket (内部 / DNS / TFTP / 用户 API) */
int  udp_open(u16 port);
void udp_close(int fd);
int  udp_sendto(int fd, u32 ip, u16 dport, const void *data, u32 len);
int  udp_recvfrom(int fd, void *buf, u32 cap, u32 timeout_ms,
                  u32 *src, u16 *sport);

/* ICMP 差错 (目的不可达/超时) 记录, 供用户程序查询 */
struct gt_icmp_err {
    u32 src;                        /* ICMP 报文来源 (通常是网关/对端) */
    u32 dst;                        /* 出错报文原目的地 */
    u16 sport, dport;               /* 内层传输层端口 */
    u8  type, code;                 /* ICMP 类型/代码 (3=不可达, 11=超时) */
    u8  proto;                      /* 内层协议 (17=UDP, 6=TCP) */
    u16 reserved;
    u32 info;                       /* ICMP 正文 (echo 标识/序列号) */
};
int  net_icmp_last_error(struct gt_icmp_err *out); /* 1=取到新差错, 0=无 */
int  udp_socket_count(void);

/* 应用层: DNS / TFTP / HTTP 文件传输 */
int  net_dns_resolve(const char *name, u32 *out);
int  net_tftp_fetch(u32 server, u16 port, const char *remote, const char *local);
int  net_http_fetch(const char *hostport, const char *path, const char *local);

/* TCP (tcp.c): 乱序重组 / 快速重传 / 拥塞控制 / RST */
int  tcp_connect(u32 ip, u16 port);
int  tcp_send(int fd, const void *buf, u32 len);
int  tcp_recv(int fd, void *buf, u32 len);   /* -1=连接被重置, 0=对端关闭 */
int  tcp_close(int fd);
int  tcp_listen(u16 port);
int  tcp_accept(int fd);
void tcp_input(u32 src, u32 dst, const u8 *seg, u32 len);
void tcp_poll(u32 now);
void tcp_icmp_error(u16 lport, u32 peer);
int  tcp_socket_count(void);

/* ------------------------------------------------------------------ 控制台 */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);
void console_clear(void);
void console_set_serial(bool on);
bool console_serial_enabled(void);
int  kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  kvprintf(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));

/* 彩色输出: 设置当前前景色 (同时驱动 VGA 与串口 ANSI) */
void gt_color(u8 fg);
void gt_color2(u8 fg, u8 bg);
void gt_color_reset(void);
void gt_bold(bool on);

/* 终端尺寸 (SYS_SCREEN) */
struct gt_screen {
    u64 cols;
    u64 rows;
};

#define GT_VERSION "GT-DOS 0.10"

/* 当前 shell 工作目录 (不含盘符, "" = 根) */
const char *shell_cwd(void);

/* 行编辑器刷新: 重画 prompt+buf, 光标停在 cur 处 */
void console_line_refresh(const char *prompt, const char *buf, int len,
                          int cur, int prev_len);
void console_line_end(void);          /* 结束行编辑, 光标移到下一行 */

/* ------------------------------------------------------------------ IDT */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct regs *r);
void idt_init(void);
void irq_install_handler(int irq, irq_handler_t fn);
void irq_uninstall_handler(int irq);
void pic_init(void);
void pic_send_eoi(int irq);
void panic(const char *msg);

/* 边界/不变量断言: 违反即 __builtin_trap (#UD -> isr6 打印并 halt) */
#define GT_ASSERT(cond)              do { if (!(cond)) __builtin_trap(); } while (0)
#define GT_ASSERT_BOUNDED(i, cap)    do { if ((u64)(i) >= (u64)(cap)) __builtin_trap(); } while (0)

/* ---------------------------------------------------- 系统调用门 (INT 0x21) */
#define SYSCALL_VECTOR 0x21
enum {
    SYS_EXIT = 0, SYS_PUTC = 1, SYS_GETC = 2, SYS_WRITE = 3, SYS_READ = 4,
    SYS_OPEN = 5, SYS_CLOSE = 6, SYS_UPTIME = 7, SYS_REBOOT = 8,
    SYS_SHUTDOWN = 9, SYS_GETTIME = 10, SYS_PUTS = 11,
    SYS_SEEK = 12, SYS_STAT = 13, SYS_OPENDIR = 14, SYS_READDIR = 15,
    SYS_SLEEP = 16, SYS_LIBLOAD = 17, SYS_LIBSYM = 18, SYS_STATX = 19,
    SYS_IPRINT = 20,
    SYS_NETINFO = 21, SYS_TCP_CONNECT = 22, SYS_TCP_SEND = 23,
    SYS_TCP_RECV = 24, SYS_TCP_CLOSE = 25, SYS_TCP_LISTEN = 26,
    SYS_TCP_ACCEPT = 27, SYS_DHCP = 28, SYS_DNS = 29,
    SYS_TFTP_GET = 30, SYS_HTTP_GET = 31,
    SYS_UDP_OPEN = 32, SYS_UDP_SENDTO = 33, SYS_UDP_RECVFROM = 34,
    SYS_ICMP_ERR = 35, SYS_UDP_CLOSE = 36, SYS_PRINTF = 37,
    SYS_CLS = 38, SYS_COLOR = 39, SYS_SCREEN = 40,
    SYS_UNLINK = 41, SYS_MKDIR = 42, SYS_RMDIR = 43,
    SYS_GETCWD = 44, SYS_HOSTNAME = 45, SYS_USERNAME = 46,
    SYS_ISADMIN = 47, SYS_VERSION = 48, SYS_UNIXTIME = 49,
    SYS_RICH = 50
};
void syscall_handle(struct regs *r);    /* 由 isr_handler 分派 */

/* --------------------------------------------------- 系统任务表 (task.c) */
#define TASK_SYS 1
#define TASK_USR 0
#define TASK_RUN 1
#define TASK_STOP 0

void task_init(void);                                  /* 登记内核服务 */
int  task_register(const char *name, u8 type, u32 mem_kb);
void task_end(int id);
int  task_count(u8 type);                              /* 0xFF = 全部 */
bool task_is_sys(int id);
int  task_kill(int id);                                /* -2=SYS 受保护 */
int  task_stop(const char *name);                      /* 按名称停止, -2=SYS */
bool task_path_is_system(const char *abs);             /* \GT-DOS\ 下的程序 */
const char *task_last_name(void);
void task_dump(void);

/* ------------------------------------------------ 程序执行模型 (exec) */
#define PROG_BASE       0x600000ULL
#define PROG_MAX        0x100000ULL
#define PROG_STACK_TOP  0x7FF000ULL
int  exec_run(const char *abs);         /* 载入并运行, 返回退出码(<0 为错误) */
void exec_exit(int code);               /* SYS_EXIT 调用: 回跳到 shell */
bool exec_is_active(void);

/* ---------------------------------------------------- 共享库 (.gtl) */
#define LIB_BASE     0x580000ULL
#define LIB_MAX      0x80000ULL
#define LIB_MAXLIBS  8
int  lib_load(const char *path);        /* 返回句柄(>=1) 或 -1 */
u64  lib_sym(int handle, const char *name);  /* 返回符号地址或 0 */
void lib_reset(void);

/* 中断嵌套状态 (idt.c) */
int  irq_in_service(void);             /* 当前正在服务的中断层数 */
int  irq_nest_max(void);               /* 历史最大嵌套深度 */
int  irq_using_apic(void);             /* 1=APIC 模式, 0=8259A */
void irq_set_apic_mode(int on);

/* ------------------------------------------------------------- 软中断 */
/* 中断上下半分离: top-half 只 raise, 耗时处理在空闲时派发执行 */
#define SOFTIRQ_KBD   0
#define SOFTIRQ_MOUSE 1
void softirq_init(void);
void softirq_set(int nr, void (*fn)(void));
void softirq_raise(int nr);
void softirq_dispatch(void);           /* 空闲循环中调用 */

/* ------------------------------------------------------- APIC (可选) */
bool apic_available(void);             /* CPUID 检测 + MMIO 探测 */
bool apic_enable(void);                /* 切到 IO-APIC + 本地 APIC */
void apic_disable(void);               /* 回退到 8259A (调用方再 pic_init) */
bool apic_active(void);
int  apic_vector_to_irq(u32 vec);      /* APIC 向量 -> IRQ 号 (查表) */
void idt_set_apic_gate(u8 vec, u64 base); /* 把桩装到指定向量 (apic.c 用) */

/* ---------------------------------------------------------------- 键盘 */
/* 普通键返回 ASCII, 扩展键返回 KEY_* 虚拟键码 (>= 0x100) */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_HOME  0x104
#define KEY_END   0x105
#define KEY_DEL   0x106
#define KEY_INS   0x107
#define KEY_PGUP  0x108
#define KEY_PGDN  0x109
#define KEY_CUP   0x10A            /* Ctrl+Up   : 终端上翻 */
#define KEY_CDOWN 0x10B            /* Ctrl+Down : 终端下翻 */
#define KEY_REBOOT 0x10C           /* Ctrl+Alt+Del */

void keyboard_init(void);
bool keyboard_has_data(void);
int  keyboard_read(void);            /* 非阻塞, 无数据返回 -1 */
int  keyboard_getchar(void);         /* 阻塞 */
void keyboard_input_byte(u8 sc);     /* 处理一个 PS/2 键盘字节 (已转码集1) */

/* ---------------------------------------------------------------- 鼠标 */
/* 滚轮事件回调: delta>0 向上滚, delta<0 向下滚 */
typedef void (*wheel_cb_t)(int delta);
void mouse_init(wheel_cb_t cb);
bool mouse_present(void);
void mouse_input_byte(u8 b);         /* 处理一个 PS/2 鼠标字节 */

/* GUI 光标状态 (640x480 坐标系) */
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);            /* bit0=左 bit1=右 bit2=中 */
bool mouse_take_delta(int *dx, int *dy); /* 取累计位移并清零 */
void mouse_warp(int x, int y);
/* 事件环: 返回 0=移动 1=按键 2=滚轮, -1=无事件; 移动/按键 a,b=坐标, 滚轮 a=量 */
int  mouse_pop_event(int *a, int *b, int *btn);

/* ------------------------------------------------------------------ Shell */
void shell_run(void);
void shell_wheel_cb(int delta);      /* 滚轮回调: 正=向上看历史 */

/* ---------------------------------------------------------------- 定时器 */
void timer_init(u32 hz);
u32  timer_ticks(void);
u32  timer_uptime_ms(void);
void timer_sleep_ms(u32 ms);

/* ------------------------------------------------------------------ 库 */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strncasecmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
void   itoa_base(u32 value, char *buf, int base, bool sign);
int    fmt_uint(u32 value, char *buf);  /* 写十进制, 返回字符数 */
int    atoi_dec(const char *s);         /* 解析十进制 (可含前导空格) */
u32    rand_next(void);

/* --------------------------------------------------------------- 时钟/RTC */
void rtc_read(int *year, int *month, int *day, int *hour, int *min, int *sec);
struct gt_time { int year, month, day, hour, min, sec; };

/* ------------------------------------------------------- 字符串扩展 (lib) */
int    isalpha_(int c);
int    isdigit_(int c);
int    isspace_(int c);
int    tolower_(int c);
int    toupper_(int c);
void   strlower(char *s);
void   strupper(char *s);
int    str_contains(const char *hay, char c);

/* ------------------------------------------------------- 通用输入层 (ui) */
/* 统一键盘输入 (PS/2 + 串口 ANSI 解码). shell / OOBE / 登录共用. */
int  ui_read_key(void);                  /* 阻塞: ASCII 或 KEY_* */
void ui_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_ok(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
void ui_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 带行编辑的输入. mask != 0 时按掩码字符显示 (密码输入).
 * history=true 启用上下方向键翻阅命令历史.
 * 返回 0=回车提交, 1=Ctrl-C 放弃, -1=Ctrl-Alt-Del. */
int  ui_read_line(const char *prompt, char *buf, int max, int mask,
                  bool history);
void ui_hist_add(const char *line);
void ui_set_scroll_cb(void (*cb)(int delta));

/* ------------------------------------------------------- 系统配置 (cfg) */
struct gt_config {
    bool oobe_done;
    u8   theme_fg;
    u8   theme_bg;
    char lang;                         /* 0=en, 1=zh */
    char last_user[16];
    char hostname[16];
    char sys_drive;                    /* 系统盘符, 默认 'C' */
};
extern struct gt_config gcfg;

bool cfg_load(void);        /* 读 GTOS.CFG; 不存在则用默认并保存 */
bool cfg_save(void);        /* 写回 GTOS.CFG */
void cfg_apply_theme(void); /* 应用主题色到控制台 */

/* ------------------------------------------------------- 用户系统 (user) */
#define USER_MAX 12
#define UNAME_MAX 16

struct gt_user {
    char name[UNAME_MAX];
    u32  hash;              /* 口令哈希 (0 = 无口令) */
    u8   flags;             /* UF_ADMIN */
};

#define UF_ADMIN 0x01

void user_init(void);                 /* 加载 USERS.SYS */
bool user_save(void);                 /* 保存 USERS.SYS */
int  user_count(void);
const struct gt_user *user_get(int i);
bool user_exists(const char *name);
bool user_add(const char *name, const char *pass, u8 flags); /* 校验+入库 */
bool user_del(const char *name);
bool user_setpass(const char *name, const char *pass);
bool user_check(const char *name, const char *pass);
const struct gt_user *user_current(void);
bool user_is_admin(void);
const char *user_name(void);
void user_login(const struct gt_user *u);
void user_logout(void);
u32  str_hash(const char *salt, const char *s);

/* 交互式登录界面 (启动时或 login 命令). 返回当前用户指针, 取消返回默认用户 */
const struct gt_user *ui_login_screen(void);

/* --------------------------------------------------------- 电源 (power) */
void power_reboot(void);   /* 不返回 */
void power_shutdown(void); /* 不返回: ACPI -> 8042 -> isa-debug-exit */

/* ------------------------------------------------------------- OOBE 向导 */
void oobe_run(void);       /* 首启向导, 完成后写 cfg */

#endif /* GT_H */