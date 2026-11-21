/* GT-DOS TC 标准库: string.h (函数已编入每个程序, include 可省略) */
#ifndef _GT_STRING_H
#define _GT_STRING_H

typedef int size_t;

int   strlen(char *s);
char *strcpy(char *d, char *s);
char *strncpy(char *d, char *s, int n);
int   strcmp(char *a, char *b);
int   strncmp(char *a, char *b, int n);
char *strcat(char *d, char *s);
char *strchr(char *s, int c);
char *strrchr(char *s, int c);
char *memcpy(char *d, char *s, int n);
char *memmove(char *d, char *s, int n);
char *memset(char *d, int c, int n);
int   memcmp(char *a, char *b, int n);

#endif
