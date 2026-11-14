; GT-DOS 示例程序: 经 INT 0x21 打印一行后退出.
; 由 exec 载入 PROG_BASE 并 jmp 进入; 平面二进制, 相对寻址, 入口在偏移 0.
BITS 64

_start:
    mov rax, 3                 ; SYS_WRITE
    mov rdi, 1                 ; fd = stdout
    lea rsi, [rel msg]
    mov rdx, msglen
    int 0x21

    mov rax, 0                 ; SYS_EXIT
    xor rdi, rdi
    int 0x21

msg:
    db "Hello from GT-DOS INT 0x21!", 10
msglen equ $ - msg