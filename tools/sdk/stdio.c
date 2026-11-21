/* GT-DOS TC 标准库: stdio.c (仅在 #include <stdio.h> 后编入程序)
   printf 由编译器生成 SYS_PRINTF, 不需要这个模块. */

int putchar(int c)
{
    putc(c);
    return c;
}

int puts(char *s)
{
    while (*s)
        putc(*s++);
    putc('\n');
    return 0;
}

int getchar(void)
{
    return getc();
}
