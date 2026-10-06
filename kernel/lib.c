/* ==========================================================================
 *  GT-DOS 内核基础库 (freestanding)
 * ========================================================================== */
#include "gt.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = (u8 *)dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = (const u8 *)a;
    const u8 *y = (const u8 *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

static char lower_of(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && lower_of(*a) == lower_of(*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(u8)lower_of(*a) - (int)(u8)lower_of(*b);
}

void itoa_base(u32 value, char *buf, int base, bool sign)
{
    static const char *digits = "0123456789abcdef";
    char tmp[36];
    int n = 0;
    bool neg = false;

    if (sign && (i32)value < 0) {
        neg = true;
        value = (u32)(-(i32)value);
    }

    if (value == 0)
        tmp[n++] = '0';
    while (value) {
        tmp[n++] = digits[value % (u32)base];
        value /= (u32)base;
    }
    if (neg)
        tmp[n++] = '-';

    int i = 0;
    while (n)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

static u32 rand_state = 0x12345678u;

u32 rand_next(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (rand_state >> 16) & 0x7FFFu;
}

/* ------------------------------------------------------------ 字符分类 */
int isalpha_(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit_(int c) { return c >= '0' && c <= '9'; }
int isspace_(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int tolower_(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper_(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

void strlower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower_((unsigned char)*s);
}

void strupper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper_((unsigned char)*s);
}

int str_contains(const char *hay, char c)
{
    for (; *hay; hay++)
        if (*hay == c)
            return 1;
    return 0;
}

int fmt_uint(u32 value, char *buf)
{
    itoa_base(value, buf, 10, false);
    return (int)strlen(buf);
}

int atoi_dec(const char *s)
{
    while (isspace_((unsigned char)*s))
        s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int v = 0;
    while (isdigit_((unsigned char)*s))
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}