; GT-DOS SDK: 最小汇编程序示例
;   ASM HELLO.S   ->  HELLO.GTP   (RUN HELLO.GTP)
; 入口即代码偏移 0; 通过 INT 0x21 调用内核 (rax = 调用号).

section .data
msg: db "Hello from ASM!", 10, 0

section .text
    mov rdi, msg            ; SYS_PUTS 参数: 字符串地址
    mov rax, 11             ; SYS_PUTS
    int 0x21

    xor edi, edi            ; 退出码 0
    xor eax, eax            ; SYS_EXIT
    int 0x21
