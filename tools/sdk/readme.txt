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
  assignments, string literals, brace/string initialisers, comments.
  Not supported: float/double, variadic functions, function pointers,
  typedef/enum/sizeof, casts, bitfields. #include lines are skipped.

Layout: 128 KB code at 0x600000, 128 KB data at 0x620000.

Builtins (see TC.H): putc, print, puts, print_int, getc, exit, sleep,
uptime, open, read, write, close, seek, stat, statx, reboot, shutdown,
tcp_connect/send/recv/close, udp_open/sendto/recvfrom/close, icmp_err,
dns, dhcp, tftp_get, http_get.

Programs run in ring0 like RUN binaries; only write files you own.
