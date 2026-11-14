; GT-DOS 示例程序 (.gtp): 演示 .gtd 数据读写 / seek / stat / .gtl 共享库
BITS 64

_start:
    ; ---- stat DATA.GTD ----
    mov rax, 13                 ; SYS_STAT
    lea rdi, [rel datapath]
    lea rsi, [rel stbuf]
    int 0x21

    ; ---- open + read 9 bytes + write ----
    mov rax, 5                  ; SYS_OPEN
    lea rdi, [rel datapath]
    xor rsi, rsi
    int 0x21
    mov r12, rax

    mov rax, 4                  ; SYS_READ
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 9
    int 0x21

    mov rax, 3                  ; SYS_WRITE
    mov rdi, 1
    lea rsi, [rel buf]
    mov rdx, 9
    int 0x21

    ; ---- seek 4, read 4, write ----
    mov rax, 12                 ; SYS_SEEK
    mov rdi, r12
    mov rsi, 4
    xor rdx, rdx
    int 0x21

    mov rax, 4
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 4
    int 0x21

    mov rax, 3                  ; "["
    mov rdi, 1
    lea rsi, [rel lbrack]
    mov rdx, 1
    int 0x21

    mov rax, 3
    mov rdi, 1
    lea rsi, [rel buf]
    mov rdx, 4
    int 0x21

    mov rax, 3                  ; "]\n"
    mov rdi, 1
    lea rsi, [rel rbrack]
    mov rdx, 2
    int 0x21

    mov rax, 6                  ; SYS_CLOSE
    mov rdi, r12
    int 0x21

    ; ---- 装载 SAMPLE.GTL, 调用 add(2,3) ----
    mov rax, 17                 ; SYS_LIBLOAD
    lea rdi, [rel libpath]
    int 0x21
    mov r12, rax
    test r12, r12
    js fail

    mov rax, 18                 ; SYS_LIBSYM
    mov rdi, r12
    lea rsi, [rel symadd]
    int 0x21
    test rax, rax
    jz fail

    mov rdi, 2
    mov rsi, 3
    call rax
    cmp rax, 5
    jne fail

    mov rax, 3
    mov rdi, 1
    lea rsi, [rel okmsg]
    mov rdx, 7
    int 0x21
    jmp done

fail:
    mov rax, 3
    mov rdi, 1
    lea rsi, [rel badmsg]
    mov rdx, 9
    int 0x21

done:
    mov rax, 16                 ; SYS_SLEEP
    mov rdi, 50
    int 0x21

    mov rax, 0                  ; SYS_EXIT
    xor rdi, rdi
    int 0x21

datapath: db "DATA.GTD", 0
libpath:  db "SAMPLE.GTL", 0
symadd:   db "add", 0
okmsg:    db "LIB OK", 10
badmsg:   db "LIB FAIL", 10
lbrack:   db "["
rbrack:   db "]", 10
stbuf:    times 16 db 0
buf:      times 32 db 0
