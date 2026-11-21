/* GT-DOS SDK: TC C compiler builtins (INT 0x21 wrappers).
 *
 * TC ignores #include lines, so this header is documentation/reference and
 * can be kept in source files for readability.
 *
 * Supported subset:
 *   int / char / pointers, 1-4D arrays, struct / union (fields, . and ->),
 *   functions (up to 6 parameters, recursion, prototypes), if/else, while,
 *   for, switch/case/default, break/continue/return, full integer
 *   expressions with ?:, prefix/postfix ++ --, compound assignments,
 *   typedef, enum, sizeof, casts, string literals, brace/string
 *   initialisers, and the builtins below.
 *
 * Not supported: float/double, variadic functions (printf is a compiler
 * builtin), function pointers and bitfields.
 *
 * A small standard library (libc.c) is compiled into every program:
 *   string.h  strlen strcpy strncpy strcmp strncmp strcat strchr strrchr
 *             memcpy memmove memset memcmp
 *   stdlib.h  malloc calloc free abs atoi rand srand
 *   ctype.h   isdigit isalpha isalnum isspace isupper islower toupper tolower
 *   stdio.h   printf putchar puts getchar (printf supports up to 5 varargs)
 *
 * Layout: 128 KB code at 0x600000, 128 KB data at 0x620000
 *         (64 KB globals, 64 KB string pool).
 */
#ifndef _GT_TC_H
#define _GT_TC_H

/* console */
void putc(int c);          /* SYS_PUTC (1)  */
void print(char *s);       /* SYS_PUTS (11), no newline */
int  print_int(int v);     /* SYS_IPRINT (20) */
int  getc(void);           /* SYS_GETC (2)  */
int  printf(char *fmt, ...);   /* SYS_PRINTF (37), compiler builtin */
int  putchar(int c);           /* libc: putc + return c */
int  puts(char *s);            /* libc: string + newline */
int  getchar(void);            /* libc: getc */

/* string.h */
int  strlen(char *s);
char *strcpy(char *d, char *s);
char *strncpy(char *d, char *s, int n);
int  strcmp(char *a, char *b);
int  strncmp(char *a, char *b, int n);
char *strcat(char *d, char *s);
char *strchr(char *s, int c);
char *strrchr(char *s, int c);
char *memcpy(char *d, char *s, int n);
char *memmove(char *d, char *s, int n);
char *memset(char *d, int c, int n);
int  memcmp(char *a, char *b, int n);

/* stdlib.h */
char *malloc(int n);
char *calloc(int n, int m);
void  free(char *p);
int   abs(int x);
int   atoi(char *s);
int   rand(void);
void  srand(int seed);

/* ctype.h */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

/* process / time */
void exit(int code);       /* SYS_EXIT (0)  */
void sleep(int ms);        /* SYS_SLEEP (16) */
int  uptime(void);         /* SYS_UPTIME (7) */

/* files (fd 0/1/2 = console) */
int  open(char *path, int flags);      /* SYS_OPEN (5) */
int  read(int fd, void *buf, int n);   /* SYS_READ (4) */
int  write(int fd, void *buf, int n);  /* SYS_WRITE (3) */
int  close(int fd);                    /* SYS_CLOSE (6) */
int  seek(int fd, int off, int whence);/* SYS_SEEK (12) */
int  stat(char *path, void *st);       /* SYS_STAT (13) */
int  statx(char *path, void *st);      /* SYS_STATX (19) */

/* power */
void reboot(void);
void shutdown(void);

/* TCP/IP (see NET.H) */
int  ip4(int a, int b, int c, int d);
int  tcp_connect(int ip, int port);
int  tcp_send(int fd, void *buf, int n);
int  tcp_recv(int fd, void *buf, int n);
int  tcp_close(int fd);
int  udp_open(int port);
int  udp_sendto(int fd, int ip, int port, void *data, int len);
int  udp_recvfrom(int fd, void *buf, int cap, int timeout_ms,
                  int *src_ip, int *src_port);
int  udp_close(int fd);
int  icmp_err(void *st);

#endif /* _GT_TC_H */
