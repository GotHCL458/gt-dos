/* GT-DOS TC 标准库: stdio.h (printf 由编译器特殊生成 SYS_PRINTF)
   printf 支持 %d %i %u %x %X %c %s %p %% , 最多 5 个可变参数,
   宽度/精度修饰会被忽略. */
#ifndef _GT_STDIO_H
#define _GT_STDIO_H

typedef int size_t;

int printf(char *fmt, ...);
int putchar(int c);
int puts(char *s);
int getchar(void);

#endif
