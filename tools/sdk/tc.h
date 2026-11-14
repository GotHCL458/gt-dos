/* GT-DOS SDK: TC C compiler builtins (INT 0x21 wrappers).
 *
 * TC ignores #include lines, so this header is documentation/reference and
 * can be kept in source files for readability.
 *
 * Supported subset:
 *   int / char / pointer variables, globals with constant initialisers,
 *   functions (up to 6 parameters, recursion), if/else, while, for,
 *   break/continue/return, + - * / % & | ^ ~ << >> comparisons,
 *   && || !, string literals, and the builtins declared below.
 */
#ifndef _GT_TC_H
#define _GT_TC_H

/* console */
void putc(int c);          /* SYS_PUTC (1)  */
void print(char *s);       /* SYS_PUTS (11) */
void puts(char *s);        /* SYS_PUTS (11) */
void print_int(int v);     /* SYS_IPRINT (20) */
int  getc(void);           /* SYS_GETC (2)  */

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

#endif /* _GT_TC_H */
