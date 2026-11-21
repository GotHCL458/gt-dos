/* GT-DOS TC 标准库: stdlib.c (仅在 #include <stdlib.h> 后编入程序) */

char __heap[16384];
int  __heap_used = 0;

char *malloc(int n)
{
    if (n <= 0)
        return 0;
    __heap_used = (__heap_used + 7) & ~7;
    if (__heap_used + n > 16384)
        return 0;
    char *p = __heap + __heap_used;
    __heap_used = __heap_used + n;
    return p;
}

void free(char *p)
{
}

char *calloc(int n, int m)
{
    int total = n * m;
    char *p = malloc(total);
    if (!p)
        return 0;
    int i = 0;
    while (i < total) {
        p[i] = 0;
        i++;
    }
    return p;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

int atoi(char *s)
{
    int sign = 1;
    int v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

int __seed = 1;

void srand(int s)
{
    __seed = s;
}

int rand(void)
{
    __seed = __seed * 1103515245 + 12345;
    return (__seed >> 16) & 0x7fff;
}
