/*
 * GT-DOS 系统头文件: 常用库函数声明
 * 注意: 内核不提供堆分配, 因此没有 malloc/free.
 */
#ifndef _GT_STDLIB_H
#define _GT_STDLIB_H

typedef unsigned long size_t;

int            abs(int v);
int            atoi(const char *s);
unsigned long  strtoul(const char *s, char **end, int base);

/* 程序退出 (等价 unistd.h 的 gt_exit) */
void gt_exit(int code);

#endif /* _GT_STDLIB_H */
