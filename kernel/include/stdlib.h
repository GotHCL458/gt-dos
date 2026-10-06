/*
 * GT-DOS 系统头文件: 常用库函数声明
 * 存放于 D:\INCLUDE\STDLIB.H
 */
#ifndef _GT_STDLIB_H
#define _GT_STDLIB_H

typedef unsigned long size_t;

void *malloc(size_t n);
void  free(void *p);
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int   atoi(const char *s);
unsigned long strtoul(const char *s, char **end, int base);

#endif /* _GT_STDLIB_H */
