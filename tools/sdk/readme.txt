GT-DOS SDK / TC
===============

TC compiles a small C subset directly to x86-64 machine code and writes a
.gtp program that RUN can execute.

  TC <src.c> [-o out.gtp]      compile (default output: current dir, .gtp)
  RUN <file.gtp>               run the program

Files:
  TC.H       builtin function reference
  HELLO.C    minimal console output example
  FIB.C      recursion + loop example

Supported C subset:
  int/char/double/pointers, 1-4D arrays, struct/union (fields, bitfields,
  . and ->), function pointers, functions with up to 6 parameters
  (recursion and prototypes supported), user variadic functions
  (va_list/va_start/va_arg/va_end), if/else, while, do/while, for,
  switch/case/default, break, continue, return, goto/labels,
  full integer and double expressions with ?:, prefix/postfix ++ --,
  compound assignments, typedef, enum, sizeof, casts, string literals,
  brace/string initialisers, object-like and function-like #define macros,
  comments.
  Not supported: long double, function-pointer arrays/return types,
  #define ## / # token pasting, #if conditionals. Other directives are skipped.

Standard library modules (compiled only when the matching header is included):
  string.h  strlen strcpy strncpy strcmp strncmp strcat strncat strchr strrchr
            strstr memcpy memmove memset memcmp
  stdlib.h  malloc calloc free strdup abs atoi strtol rand srand (16 KB heap)
  ctype.h   isdigit isalpha isalnum isspace isupper islower toupper tolower
  stdio.h   printf putchar puts getchar
  stdarg.h  va_list (va_start / va_arg / va_end are compiler builtins)
  printf: %d %i %u %x %X %c %s %p %f %%, up to 5 varargs.

Layout: 128 KB code at 0x600000, 128 KB data at 0x620000.

Builtins (see TC.H): putc, print, print_int, getc, exit, sleep,
uptime, open, read, write, close, seek, stat, statx, reboot, shutdown,
tcp_connect/send/recv/close, udp_open/sendto/recvfrom/close, icmp_err,
dns, dhcp, tftp_get, http_get.

Programs run in ring0 like RUN binaries; only write files you own.
