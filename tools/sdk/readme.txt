GT-DOS SDK / TC
===============

TC compiles a small C subset directly to x86-64 machine code and writes a
.gtp program that RUN can execute.

  TC <src.c> [more.c ...] [-o out.gtp]
                               compile (default output: current dir, .gtp)
  RUN <file.gtp>               run the program

Multiple source files are flattened automatically in the given order into one
program: they share globals, functions, typedefs, enums and macros, calls are
resolved after all modules are parsed, and a header included by several modules
is expanded only once. Each module still resolves "..." includes relative to
its own directory. Up to 4 sources (32 KB total).

Optimization:
  -O0 .. -O4    levels (default -O0); -Os size, -Ofast = -O4
  -fconst       constant folding + immediate operands   (O1)
  -fstrength    strength reduction                      (O2)
  -fdce         constant-if dead branch elimination     (O3)
  -floop        constant-loop elimination               (O4)
  -fno-X        disable a pass; -fdce implies -fconst, -floop implies -fdce,
                -fno-const disables all; later flags win
  -g            debug (reset to -O0);  -v  print effective flags + sizes
  All levels use direct local access, shortest immediates (xor/imm32/imm8) and
  emit cmp+jcc directly for conditions.

Files:
  TC.H       builtin function reference
  NET.H      network builtins and helper library API
  NET.C      network helper library (compiled on demand)
  HELLO.C    minimal console output example
  FIB.C      recursion + loop example

Supported C subset:
  int/char/double/pointers, 1-4D arrays, struct/union (fields, bitfields,
  . and ->), function pointers and function-pointer arrays, functions with up
  to 6 parameters (recursion and prototypes supported), user variadic functions
  (va_list/va_start/va_arg/va_end), if/else, while, do/while, for,
  switch/case/default, break, continue, return, goto/labels,
  full signed/unsigned integer and double expressions with ?:, prefix/postfix
  ++ --, compound assignments, typedef, enum, sizeof(type)/sizeof expr, casts,
  string/char literals with \xNN and octal escapes, brace/string initialisers,
  object-like and function-like #define macros (including # and ##), comments.
  Not supported: long double, functions returning function pointers,
  #if conditionals. Other directives are skipped.

Standard library modules (compiled only when the matching header is included):
  string.h  strlen strcpy strncpy strcmp strncmp strcat strncat strchr strrchr
            strstr memcpy memmove memset memcmp
  stdlib.h  malloc calloc free strdup abs atoi strtol rand srand (16 KB heap)
  ctype.h   isdigit isalpha isalnum isspace isupper islower toupper tolower
  stdio.h   printf putchar puts getchar
  stdarg.h  va_list (va_start / va_arg / va_end are compiler builtins)
  net.h     TCP/UDP builtins + NET.C helpers (net_aton/net_resolve/
            net_connect/net_send_str/net_send_all/net_recv_line/net_recv_all)
  printf: %d %i %u %x %X %c %s %p %f %%, up to 5 varargs.

Layout: 128 KB code at 0x600000, 128 KB data at 0x620000.

Builtins (see TC.H): putc, print, print_int, getc, exit, sleep,
uptime, open, read, write, close, seek, stat, statx, reboot, shutdown,
cls, color, screen, getcwd, hostname, username, isadmin, version,
unixtime, rich, unlink, mkdir, rmdir,
tcp_connect/send/recv/close, udp_open/sendto/recvfrom/close, icmp_err,
dns, dhcp, tftp_get, http_get.

Programs run in ring0 like RUN binaries; only write files you own.
