; 程序执行切换: 保存 shell 栈帧, 跳到程序; SYS_EXIT 经 exec_longjmp 回跳.
BITS 64
section .text
global exec_enter
global exec_longjmp

; u64 exec_enter(u64 entry, u64 stack_top, u64 *ctx)
; ctx: [0]=rsp, [8]=rbp, [16]=resume_rip
exec_enter:
    mov [rdx+0], rsp
    mov [rdx+8], rbp
    lea rax, [rel .resume]
    mov [rdx+16], rax
    mov rsp, rsi
    xor rbp, rbp
    jmp rdi
.resume:
    ret                       ; rax = 退出码 (由 exec_longjmp 设置)

; void exec_longjmp(u64 *ctx, u64 code) -- 从中断上下文回跳 shell
exec_longjmp:
    sti                       ; 软件中断清除了 IF, 回跳前恢复开中断
    mov rax, rsi
    mov rsp, [rdi+0]
    mov rbp, [rdi+8]
    jmp qword [rdi+16]