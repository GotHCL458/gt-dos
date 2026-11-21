; 多线程调度相关的汇编: yield 门、线程入口蹦床、xmm/mxcsr 保存恢复
BITS 64
section .text

extern isr_common
extern thread_exit_current

; ---- INT 0x2F: 主动让出 (与硬件 IRQ 使用相同的帧格式) ----
global schedyield
schedyield:
    push qword 0
    push qword 0x2F
    jmp isr_common

; ---- 新线程首次进入: rdi=入口函数, rsi=参数 ----
; 按 TC 调用约定把参数压栈后 call, 返回后以返回值退出线程.
global thread_entry_asm
thread_entry_asm:
    push rsi                    ; TC 函数参数位于 [rbp+16]
    call rdi
    add rsp, 8
    mov rdi, rax                ; 退出码 = 入口函数返回值
    sub rsp, 8                  ; 保持 SysV call 前 16 字节对齐
    call thread_exit_current
    hlt

; ---- xmm0..7 + mxcsr 保存/恢复 (rdi = 136 字节缓冲) ----
global sched_xmm_save
sched_xmm_save:
    movdqu [rdi], xmm0
    movdqu [rdi + 16], xmm1
    movdqu [rdi + 32], xmm2
    movdqu [rdi + 48], xmm3
    movdqu [rdi + 64], xmm4
    movdqu [rdi + 80], xmm5
    movdqu [rdi + 96], xmm6
    movdqu [rdi + 112], xmm7
    stmxcsr [rdi + 128]
    ret

global sched_xmm_load
sched_xmm_load:
    movdqu xmm0, [rdi]
    movdqu xmm1, [rdi + 16]
    movdqu xmm2, [rdi + 32]
    movdqu xmm3, [rdi + 48]
    movdqu xmm4, [rdi + 64]
    movdqu xmm5, [rdi + 80]
    movdqu xmm6, [rdi + 96]
    movdqu xmm7, [rdi + 112]
    ldmxcsr [rdi + 128]
    ret
