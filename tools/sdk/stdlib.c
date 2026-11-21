/* GT-DOS TC 标准库: stdlib.c (仅在 #include <stdlib.h> 后编入程序) */

char __heap[16384];
int  __heap_used = 0;
int  __heap_mx = 0;                     /* 惰性创建的多线程堆锁 */

char *malloc(int n)
{
    char *p = 0;
    if (n <= 0)
        return 0;
    if (__heap_mx == 0)
        __heap_mx = mutex_create();
    if (__heap_mx > 0)
        mutex_lock(__heap_mx);
    __heap_used = (__heap_used + 7) & ~7;
    if (__heap_used + n <= 16384) {
        p = __heap + __heap_used;
        __heap_used = __heap_used + n;
    }
    if (__heap_mx > 0)
        mutex_unlock(__heap_mx);
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

char *strdup(char *s)
{
    int n = 0;
    while (s[n])
        n++;
    char *p = malloc(n + 1);
    if (!p)
        return 0;
    int i = 0;
    while (i <= n) {
        p[i] = s[i];
        i++;
    }
    return p;
}

int strtol(char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
            s++;
        } else {
            base = 10;
        }
    }
    int v = 0;
    for (;;) {
        int c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base)
            break;
        v = v * base + d;
        s++;
    }
    if (end)
        *end = s;
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
