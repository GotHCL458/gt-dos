/* GT-DOS TC 标准库: string.c (仅在 #include <string.h> 后编入程序) */

int strlen(char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

char *strcpy(char *d, char *s)
{
    char *r = d;
    while (*s)
        *d++ = *s++;
    *d = 0;
    return r;
}

char *strncpy(char *d, char *s, int n)
{
    char *r = d;
    int i = 0;
    while (i < n && s[i]) {
        d[i] = s[i];
        i++;
    }
    while (i < n) {
        d[i] = 0;
        i++;
    }
    return r;
}

int strcmp(char *a, char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

int strncmp(char *a, char *b, int n)
{
    while (n > 0 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return *a - *b;
}

char *strcat(char *d, char *s)
{
    char *r = d;
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
    return r;
}

char *strchr(char *s, int c)
{
    while (*s) {
        if (*s == c)
            return s;
        s++;
    }
    if (c == 0)
        return s;
    return 0;
}

char *strrchr(char *s, int c)
{
    char *r = 0;
    while (*s) {
        if (*s == c)
            r = s;
        s++;
    }
    if (c == 0)
        return s;
    return r;
}

char *strstr(char *h, char *n)
{
    if (!*n)
        return h;
    while (*h) {
        char *a = h;
        char *b = n;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return h;
        h++;
    }
    return 0;
}

char *strncat(char *d, char *s, int n)
{
    char *r = d;
    while (*d)
        d++;
    while (n > 0 && *s) {
        *d++ = *s++;
        n--;
    }
    *d = 0;
    return r;
}

int memcmp(char *a, char *b, int n)
{
    while (n > 0) {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
        n--;
    }
    return 0;
}

char *memcpy(char *d, char *s, int n)
{
    char *r = d;
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
    return r;
}

char *memmove(char *d, char *s, int n)
{
    char *r = d;
    if (d < s) {
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    } else {
        d = d + n;
        s = s + n;
        while (n > 0) {
            d--;
            s--;
            *d = *s;
            n--;
        }
    }
    return r;
}

char *memset(char *d, int c, int n)
{
    char *r = d;
    while (n > 0) {
        *d++ = (char)c;
        n--;
    }
    return r;
}
