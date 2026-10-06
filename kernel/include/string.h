/*
 * GT-DOS 系统头文件: 字符串操作声明
 * 存放于 D:\INCLUDE\STRING.H
 */
#ifndef _GT_STRING_H
#define _GT_STRING_H

typedef unsigned long size_t;

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

#endif /* _GT_STRING_H */
