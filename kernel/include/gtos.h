/*
 * GT-DOS 内核 API 摘要 (供用户程序参考), 存放于 C:\GT-DOS\INCLUDE\GTOS.H.
 *
 * 系统调用经 INT 0x21 进入内核: 调用号放 rax, 参数按 SysV 顺序放
 * rdi/rsi/rdx/r10/r8/r9, 返回值写回 rax. 程序运行于 ring0, 本门是 API
 * 入口而非权限边界; 文件写仍受登录用户的家目录约束.
 *
 *   号  名称        rdi/rsi/rdx 含义                     返回
 *   0   SYS_EXIT    code                                   不返回(回跳 shell)
 *   1   SYS_PUTC    ch                                     0
 *   2   SYS_GETC    -                                      键码
 *   3   SYS_WRITE   fd, buf, n                             写入字节数
 *   4   SYS_READ    fd, buf, n                             读取字节数
 *   5   SYS_OPEN    path, flags                            fd (>=3) 或 -1
 *   6   SYS_CLOSE   fd                                     0 或 -1
 *   7   SYS_UPTIME  -                                      毫秒
 *   8   SYS_REBOOT  -                                      管理员才执行
 *   9   SYS_SHUTDOWN-                                      管理员才执行
 *   10  SYS_GETTIME gt_time*                               0 或 -1
 *   11  SYS_PUTS    str                                    0
 *   12  SYS_SEEK    fd, offset, whence                    新位置或 -1
 *   13  SYS_STAT    path, gt_stat*                         0 或 -1
 *   14  SYS_OPENDIR path                                   fd 或 -1
 *   15  SYS_READDIR fd, gt_dirent*                         1/0/-1
 *   16  SYS_SLEEP   ms                                     0
 *   17  SYS_LIBLOAD path (.gtl)                            句柄或 -1
 *   18  SYS_LIBSYM  handle, name                           地址或 0
 *   19  SYS_STATX   path, gt_statx*                        0 或 -1
 *   20  SYS_IPRINT  value                                  0 (打印十进制)
 *   21  SYS_NETINFO net_info*                              0 或 -1
 *   22  SYS_TCP_CONNECT ip, port                           socket 或 -1
 *   23  SYS_TCP_SEND fd, buf, n                            发送字节数
 *   24  SYS_TCP_RECV fd, buf, n                            接收字节数 (0=EOF)
 *   25  SYS_TCP_CLOSE fd                                   0
 *   26  SYS_TCP_LISTEN port                                socket 或 -1
 *   27  SYS_TCP_ACCEPT fd                                  0 或 -1
 *   28  SYS_DHCP    -                                      0 或 -1
 *   29  SYS_DNS     name                                   IPv4 或 0
 *   30  SYS_TFTP_GET server, remote, local                 字节数或 -1
 *   31  SYS_HTTP_GET hostport, path, local                 字节数或 -1
 *   32  SYS_UDP_OPEN port                                 socket 或 -1
 *   33  SYS_UDP_SENDTO fd, ip, port, data, len            字节数或 -1
 *   34  SYS_UDP_RECVFROM fd, buf, cap, timeout, src, sport 字节数或 -1
 *   35  SYS_ICMP_ERR st                                    1/0
 *   36  SYS_UDP_CLOSE fd                                   0
 *   37  SYS_PRINTF  fmt, nargs, argptr                     0
 *   38  SYS_CLS     -                                      0
 *   39  SYS_COLOR   fg, bg                                 0
 *   40  SYS_SCREEN  gt_screen*                             0 或 -1
 *   41  SYS_UNLINK  path                                   0 或 -1
 *   42  SYS_MKDIR   path                                   0 或 -1
 *   43  SYS_RMDIR   path                                   0 或 -1
 *   44  SYS_GETCWD  buf, cap                               长度或 -1
 *   45  SYS_HOSTNAME buf, cap                              长度或 -1
 *   46  SYS_USERNAME buf, cap                              长度或 -1
 *   47  SYS_ISADMIN -                                      0/1
 *   48  SYS_VERSION buf, cap                               长度或 -1
 *   49  SYS_UNIXTIME-                                      Unix 秒或 -1
 *   50  SYS_RICH    md, len, title                         0 或 -1
 *   64  SYS_GFX_INFO  gt_gui_info*                         0 或 -1
 *   65  SYS_GUI_BEGIN -                                    0 或 -1 (需桌面)
 *   66  SYS_GUI_END   -                                    0
 *   67  SYS_GFX_FILL  x, y, w, h, color                     0
 *   68  SYS_GFX_FRAME x, y, w, h, color                     0
 *   69  SYS_GFX_TEXT  x, y, str, color                      文本像素宽
 *   70  SYS_GFX_CLEAR color                                 0
 *   71  SYS_GUI_FLUSH -                                    0
 *   72  SYS_GUI_POLL  gt_gui_event*                         0 或 -1
 *   73  SYS_GFX_BAR   x, y, w, h, pct, color                0
 *
 * GUI 画布: 先 GUI_BEGIN 接管全屏 (需已进入桌面), 用 FILL/FRAME/TEXT/BAR
 * 画到后台缓冲, GUI_FLUSH 提交, GUI_POLL 读取鼠标/键盘, 最后 GUI_END 恢复
 * 桌面. 配套 C 库见 <GUI.H> (gt_panel/gt_button/gt_progress/gt_checkbox/
 * gt_radio). 6 参数调用经 r10/r8/r9 传递.
 *
 * SYS_RICH 直接用 Markdown 子集把一段文本渲染到控制台 (标题/粗体/斜体/
 * 删除线/代码/链接/图片/引用/列表/任务/表格/围栏代码块).
 *
 * SYS_STAT 返回 {size, attr, is_dir}; SYS_STATX 额外带 exFAT 厂商扩展:
 * flags (0x01 = 内容加密), version, crc32, owner[16], mode.
 *
 * fd 0/1/2 为控制台(stdin/stdout/stderr), 文件 fd 从 3 起.
 * 文件系统为 exFAT; 配套头: unistd.h / stat.h / dirent.h / gtlib.h /
 * time.h / stdint.h / ctype.h / stdio.h / string.h / stdlib.h.
 * 系统内 C 编译器 TC: TC <源.c> [-o 输出.gtp], SDK 位于 \SDK\.
 * 网络: RTL8139 + ARP/IPv4/ICMP/UDP/DHCP/TCP; 默认 10.0.2.15/24.
 */
#ifndef _GT_GTOS_H
#define _GT_GTOS_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;

/* 控制台 */
void console_putc(char c);
void console_puts(const char *s);

/* 颜色 (16 色 VGA 调色板) */
enum {
    C_BLACK = 0, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN,
    C_LGRAY, C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA,
    C_YELLOW, C_WHITE
};

/* 文件 (经 INT 0x21, 见 unistd.h) */
int  gt_open(const char *path, int flags);
int  gt_read(int fd, void *buf, unsigned n);
int  gt_write(int fd, const void *buf, unsigned n);
int  gt_close(int fd);

/* 终端尺寸与样式 */
typedef struct { unsigned long cols, rows; } gt_screen_t;
int  gt_screen(gt_screen_t *sc);
void gt_cls(void);
void gt_setcolor(int fg, int bg);

/* 目录/文件操作 */
int  gt_unlink(const char *path);
int  gt_mkdir(const char *path);
int  gt_rmdir(const char *path);

/* 系统信息 */
int  gt_getcwd(char *buf, unsigned cap);
int  gt_hostname(char *buf, unsigned cap);
int  gt_username(char *buf, unsigned cap);
int  gt_isadmin(void);
int  gt_version(char *buf, unsigned cap);
long gt_unixtime(void);

/* 富文本 (Markdown 子集) */
int  gt_rich(const char *md, unsigned len, const char *title);

/* ------------------------------------------------------- GUI 画布 / 绘图 */
struct gt_gui_info { long w, h, scale, cw, ch; };
struct gt_gui_event { long x, y, buttons, key; };

int  gt_gfx_info(struct gt_gui_info *gi);
int  gt_gui_begin(void);
void gt_gui_end(void);
void gt_gfx_clear(unsigned color);
void gt_gfx_fill(int x, int y, int w, int h, unsigned color);
void gt_gfx_frame(int x, int y, int w, int h, unsigned color);
int  gt_gfx_text(int x, int y, const char *s, unsigned color);
void gt_gfx_bar(int x, int y, int w, int h, int pct, unsigned color);
void gt_gui_flush(void);
int  gt_gui_poll(struct gt_gui_event *ev);

/* 进程/电源 */
void gt_exit(int code);
void reboot(void);
void shutdown(void);

#endif /* _GT_GTOS_H */