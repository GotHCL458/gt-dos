/*
 * GT-DOS 内核 API 摘要 (供用户程序参考)
 * 存放于 D:\INCLUDE\GTOS.H
 * ---------------------------------------------------------------------
 * 这些接口目前以内核内置命令形式提供; 未来的 GT-DOS 用户程序
 * 将通过 INT 0x21 风格的系统调用使用它们.
 */
#ifndef _GT_GTOS_H
#define _GT_GTOS_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;

/* 控制台 */
void console_putc(char c);
void console_puts(const char *s);
int  kprintf(const char *fmt, ...);

/* 颜色 (16 色 VGA 调色板) */
enum {
    C_BLACK = 0, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN,
    C_LGRAY, C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA,
    C_YELLOW, C_WHITE
};
void gt_color(u8 fg);
void gt_color_reset(void);

/* 文件 (FAT, 盘符 C: D:) */
int  open(const char *path);
int  read(int fd, void *buf, unsigned n);
int  write(int fd, const void *buf, unsigned n);
int  close(int fd);

/* 进程/电源 */
void exit(int code);
void reboot(void);
void shutdown(void);

#endif /* _GT_GTOS_H */
