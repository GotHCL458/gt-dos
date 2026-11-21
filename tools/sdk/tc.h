/* GT-DOS SDK: TC C compiler builtins (INT 0x21 wrappers).
 *
 * TC resolves #include at compile time, so this header can be included for
 * readability and to pull in the matching standard-library module.
 *
 * Supported subset:
 *   int / char / double / pointers, 1-4D arrays, struct / union (fields, bitfields,
 *   . and ->), function pointers and function-pointer arrays, functions
 *   (up to 6 parameters, recursion, prototypes, user variadic with
 *   va_list/va_start/va_arg/va_end), if/else, while, do/while, for,
 *   switch/case/default, break/continue/return/goto, full expressions
 *   (signed/unsigned integer and double) with ?:, prefix/postfix ++ --,
 *   compound assignments, typedef, enum, sizeof(type)/sizeof expr, casts,
 *   string/char literals with \xNN and octal escapes, brace/string
 *   initialisers, object-like and function-like #define macros with # and ##,
 *   and the builtins below.
 *
 * Not supported: long double, functions returning function pointers, and
 * #if conditionals.
 *
 * Multiple .c files may be passed to TC; they are flattened automatically in
 * the given order into one program (shared symbols/macros, include-once).
 * Expression temporaries use r11/r10 with deferred saves; deeper expressions
 * spill to the stack, and user calls only save live temporaries.
 *
 * Threads (SYS 51..59): thread_create(fn, arg) -> tid, yield(), thread_exit(code),
 * thread_join(tid) (blocking), gettid(), mutex_create/lock/unlock, thread_detach.
 * sleep(ms) blocks only the calling thread. Threads share the program's memory;
 * the kernel preempts on the timer unless a system call is in progress.
 *
 * Standard library modules are compiled in only when #included:
 *   string.h  strlen strcpy strncpy strcmp strncmp strcat strncat strchr strrchr
 *             strstr memcpy memmove memset memcmp
 *   stdlib.h  malloc calloc free strdup abs atoi strtol rand srand
 *   ctype.h   isdigit isalpha isalnum isspace isupper islower toupper tolower
 *   stdio.h   printf putchar puts getchar (printf supports up to 5 varargs)
 *   stdarg.h  va_list (va_start/va_arg/va_end are compiler builtins)
 *   net.h     TCP/UDP builtins + net.c helpers (net_connect/net_send_str/...)
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
char *strstr(char *hay, char *needle);
char *strncat(char *d, char *s, int n);
char *memcpy(char *d, char *s, int n);
char *memmove(char *d, char *s, int n);
char *memset(char *d, int c, int n);
int  memcmp(char *a, char *b, int n);

/* stdlib.h */
char *malloc(int n);
char *calloc(int n, int m);
void  free(char *p);
char *strdup(char *s);
int   abs(int x);
int   atoi(char *s);
int   strtol(char *s, char **end, int base);
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

/* console / UI (SYS_CLS = 38, SYS_COLOR = 39, SYS_SCREEN = 40) */
void cls(void);
void color(int fg, int bg);
int  screen(void *sc);                 /* sc = { int cols; int rows; } */

/* filesystem (SYS_UNLINK = 41, SYS_MKDIR = 42, SYS_RMDIR = 43) */
int  unlink(char *path);
int  mkdir(char *path);
int  rmdir(char *path);

/* system info (SYS_GETCWD = 44 .. SYS_UNIXTIME = 49) */
int  getcwd(char *buf, int cap);
int  hostname(char *buf, int cap);
int  username(char *buf, int cap);
int  isadmin(void);
int  version(char *buf, int cap);
int  unixtime(void);

/* rich text: 用 Markdown 子集渲染到控制台 (SYS_RICH = 50) */
int  rich(char *md, int len, char *title);

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
