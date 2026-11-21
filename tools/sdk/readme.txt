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
  int/char/pointers, 1-4D arrays, struct/union (fields, . and ->),
  functions with up to 6 parameters (recursion and prototypes supported),
  if/else, while, for, switch/case/default, break, continue, return,
  full integer expressions with ?:, prefix/postfix ++ --, compound
  assignments, typedef, enum, sizeof, casts, string literals,
  brace/string initialisers, comments.
  Not supported: float/double, variadic functions (printf is built in),
  function pointers, bitfields. #include lines are skipped.

Standard library (libc.c, compiled into every program):
  string.h  strlen strcpy strncpy strcmp strncmp strcat strchr strrchr
            memcpy memmove memset memcmp
  stdlib.h  malloc calloc free abs atoi rand srand (16 KB heap)
  ctype.h   isdigit isalpha isalnum isspace isupper islower toupper tolower
  stdio.h   printf putchar puts getchar
  printf: %d %i %u %x %X %c %s %p %%, up to 5 varargs.

Layout: 128 KB code at 0x600000, 128 KB data at 0x620000.

Builtins (see TC.H): putc, print, print_int, getc, exit, sleep,
uptime, open, read, write, close, seek, stat, statx, reboot, shutdown,
tcp_connect/send/recv/close, udp_open/sendto/recvfrom/close, icmp_err,
dns, dhcp, tftp_get, http_get.

Programs run in ring0 like RUN binaries; only write files you own.
