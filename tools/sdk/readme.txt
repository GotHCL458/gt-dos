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
  int/char/pointer variables, global variables with constant initialisers,
  functions with up to 6 parameters (recursion supported),
  if/else, while, for, break, continue, return,
  arithmetic and bitwise operators, comparisons, && || !,
  string literals, comments, and #include lines are skipped.

Builtins (see TC.H): putc, print, puts, print_int, getc, exit, sleep,
uptime, open, read, write, close, seek, stat, statx, reboot, shutdown,
tcp_connect/send/recv/close, udp_open/sendto/recvfrom/close, icmp_err,
dns, dhcp, tftp_get, http_get.

Programs run in ring0 like RUN binaries; only write files you own.
