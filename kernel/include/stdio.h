/*
 * GT-DOS 系统头文件: 标准 I/O 声明 (面向 GT-DOS 用户程序)
 * 注意: 这是 GT-DOS 自带的参考头文件, 存放于 D:\INCLUDE\STDIO.H
 */
#ifndef _GT_STDIO_H
#define _GT_STDIO_H

typedef unsigned long size_t;

int  printf(const char *fmt, ...);
int  puts(const char *s);
int  putchar(int c);
int  sprintf(char *buf, const char *fmt, ...);

#endif /* _GT_STDIO_H */
