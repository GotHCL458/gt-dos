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

/* 进程/电源 */
void gt_exit(int code);
void reboot(void);
void shutdown(void);

#endif /* _GT_GTOS_H */