/* GT-DOS TC 标准库: stdlib.h (函数已编入每个程序, include 可省略) */
#ifndef _GT_STDLIB_H
#define _GT_STDLIB_H

typedef int size_t;

char *malloc(int n);
char *calloc(int n, int m);
void  free(char *p);
char *strdup(char *s);
int   abs(int x);
int   atoi(char *s);
int   strtol(char *s, char **end, int base);
int   rand(void);
void  srand(int seed);

#endif
