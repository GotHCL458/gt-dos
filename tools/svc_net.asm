; GT-DOS 系统服务示例: 以 SYS 任务身份运行的 .gtp 程序.
; 打印服务名与运行时长, 然后正常退出. 由 SERVICE RUN 启动.
BITS 64

_start:
    mov rax, 11                 ; SYS_PUTS
    lea rdi, [rel banner]
    int 0x21

    mov rax, 7                  ; SYS_UPTIME (ms)
    int 0x21
    mov rdi, rax
    mov rax, 20                 ; SYS_IPRINT
    int 0x21

    mov rax, 11                 ; SYS_PUTS
    lea rdi, [rel tail]
    int 0x21

    mov rax, 0                  ; SYS_EXIT
    xor rdi, rdi
    int 0x21

banner:
    db "GT-DOS service: net agent, uptime ", 0
tail:
    db " ms", 10, 0
