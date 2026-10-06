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

/* 空闲等待: hlt 唤醒后顺便派发软中断 (bottom-half 在非中断上下文执行) */
static inline void cpu_relax(void)
{
    cpu_halt();
    softirq_dispatch();
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
#define MB_FONT_LBA   0x0604       /* u32 font.bin 起始 LBA */
#define MB_FONT_SECT  0x0608       /* u32 font.bin 扇区数 */
#define MB_CPM_LBA    0x060C       /* u32 cpm.bin 起始 LBA */
#define MB_CPM_SECT   0x0610       /* u32 cpm.bin 扇区数 */

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
struct gfx_mode { int w, h; };
bool gfx_available(void);            /* Bochs VBE 可用? */
bool gfx_init(void);                 /* 进入 640x480x32 + 双缓冲 */
void gfx_shutdown(void);
bool gfx_active(void);
int  gfx_w(void);                    /* 当前分辨率宽 */
int  gfx_h(void);                    /* 当前分辨率高 */
bool gfx_set_mode(int w, int h);     /* 切换分辨率 */
extern const struct gfx_mode gfx_modes[];
int  gfx_mode_count(void);
void gfx_mode_size(int idx, int *w, int *h);
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
void gui_run(void);                  /* 进入图形桌面 (阻塞, 退出返回 shell) */
bool gui_available(void);            /* VBE Bochs 接口可用? */
bool gui_active(void);               /* 当前在图形桌面中 (屏蔽滚轮回 shell) */
void vga_enter_gfx(void);            /* 控制台切到图形文本后端 */
void vga_refresh(void);              /* 重绘整个文本控制台 */
void vga_relayout(void);             /* 分辨率改变后重排文本网格 */

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

/* ------------------------------------------------------------------ 控制台 */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);
void console_clear(void);
void console_set_serial(bool on);
bool console_serial_enabled(void);
int  kprintf(const char *fmt, ...);
int  kvprintf(const char *fmt, va_list ap);

/* 彩色输出: 设置当前前景色 (同时驱动 VGA 与串口 ANSI) */
void gt_color(u8 fg);
void gt_color2(u8 fg, u8 bg);
void gt_color_reset(void);
void gt_bold(bool on);

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
void   itoa_base(u32 value, char *buf, int base, bool sign);
int    fmt_uint(u32 value, char *buf);  /* 写十进制, 返回字符数 */
int    atoi_dec(const char *s);         /* 解析十进制 (可含前导空格) */
u32    rand_next(void);

/* --------------------------------------------------------------- 时钟/RTC */
void rtc_read(int *year, int *month, int *day, int *hour, int *min, int *sec);

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