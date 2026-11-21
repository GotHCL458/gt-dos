; GT-DOS 中断/异常桩 (x86-64 长模式).
; CPU 异常 0..31 与硬件 IRQ 统一进入 isr_common, 再调用 C 处理函数.
; 桩不再 cli: 是否允许嵌套由 C 层 (isr_handler) 决定, 高优先级才能打断低优先级.
; IRQ 两套向量: PIC 模式 48..63 (irqN), APIC 模式 64..79+ (airqN).
; 系统调用门 INT 0x21 占用向量 33 (PIC 已重映射到 48+, 32..47 空闲).

BITS 64
section .text
extern isr_handler
extern sched_next_sp

; ---- 无错误码的异常: 压入占位错误码 ----
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; ---- CPU 自动压入错误码的异常 ----
%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; ---- 硬件 IRQ, PIC 重映射后位于 48..63 (腾出 0x21 给系统调用门) ----
%macro IRQ 2
global irq%1
irq%1:
    push qword 0
    push qword %2                       ; 向量号 48+n
    jmp isr_common
%endmacro

IRQ 0,  48
IRQ 1,  49
IRQ 2,  50
IRQ 3,  51
IRQ 4,  52
IRQ 5,  53
IRQ 6,  54
IRQ 7,  55
IRQ 8,  56
IRQ 9,  57
IRQ 10, 58
IRQ 11, 59
IRQ 12, 60
IRQ 13, 61
IRQ 14, 62
IRQ 15, 63

; ---- 硬件 IRQ, APIC 模式 (向量按优先级类分配, 与 apic.c irq_vec 一致) ----
%macro AIRQ 2
global airq%1
airq%1:
    push qword 0
    push qword %2                       ; 向量号 (高 4 位 = 优先级类)
    jmp isr_common
%endmacro

AIRQ 0,  144                            ; timer  (类9, 最紧急)
AIRQ 1,  128                            ; kbd    (类8)
AIRQ 2,  64                             ; cascade(类4)
AIRQ 3,  65
AIRQ 4,  80                             ; com1   (类5)
AIRQ 5,  66
AIRQ 6,  81                             ; floppy (类5)
AIRQ 7,  67
AIRQ 8,  68
AIRQ 9,  69
AIRQ 10, 96                             ; 类6
AIRQ 11, 70
AIRQ 12, 129                            ; mouse  (类8)
AIRQ 13, 71
AIRQ 14, 112                            ; ata0   (类7)
AIRQ 15, 113                            ; ata1   (类7)

; ---- 杂散中断 (spurious, 向量 255): 无需 EOI, 直接返回 ----
global isrspl
isrspl:
    push qword 0
    push qword 255
    jmp isr_common

; ---- 系统调用门 INT 0x21 (向量 33): 交由 isr_handler 分派到 syscall_handle ----
global sysint
sysint:
    push qword 0
    push qword 33
    jmp isr_common

; ----------------------------------------------------------------------------
global isr_common
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15                            ; 15 个寄存器 -> struct regs 前 15 项

    mov rdi, rsp                        ; 参数: struct regs *
    mov rbx, rsp                        ; rbx 保存 regs 起始 (原值已在栈上)
    and rsp, -16                        ; SysV ABI: call 前 rsp 须 16 字节对齐
    call isr_handler
    mov rsp, rbx                        ; 恢复到 regs 起始

    ; 调度器请求切换: 改用另一线程保存的同样格式的帧
    mov rax, [rel sched_next_sp]
    test rax, rax
    jz .no_switch
    mov rsp, rax
    mov qword [rel sched_next_sp], 0
.no_switch:

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16                         ; 丢弃 int_no 与 err_code
    iretq

; ----------------------------------------------------------------------------
; 读取控制寄存器的小工具
global gt_read_cr2
gt_read_cr2:
    mov rax, cr2
    ret
