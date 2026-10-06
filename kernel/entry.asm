; ============================================================================
;  GT-DOS 内核入口 (x86-64 长模式)
;  引导扇区以 16 位实模式跳转到 _start (物理地址 0x10000, ds=0)
;  流程: 实模式 -> 32 位保护模式 -> 建恒等页表 -> PAE + EFER.LME + 分页
;        -> 64 位长模式 -> kmain
; ============================================================================

BITS 16
section .boot16
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    ; ---- 打开 A20 地址线 (Fast A20 Gate) ----
    in  al, 0x92
    or  al, 0000_0010b
    out 0x92, al

    ; ---- 载入临时 32 位 GDT (栈上构造 6 字节伪描述符) ----
    mov  eax, gdt32
    push dword eax
    push word GDT32_LIMIT
    o32 lgdt [esp]
    add  sp, 6

    ; ---- 进入 32 位保护模式 ----
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp dword 0x08:pm_start

; ----------------------------------------------------------------------------
BITS 32
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 清零 .bss (含页表与栈) ----
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    ; ============================================================
    ;  建立恒等页表 (2MB 大页):
    ;    PML4[0] -> PDPT ; PML4 其余为空
    ;    PDPT[0] -> pd_low  : 0x00000000..0x01FFFFFF (32MB RAM)
    ;    PDPT[3] -> pd_hi   : 0xC0000000..0xFFFFFFFF (MMIO/LFB)
    ;  条目格式 (64 位): 低 32 位 = 地址|标志, 高 32 位 = 0
    ;    标志: 0x03=Present|PS(2MB页); 0x83=Present|2MB; 0x8F=+PCD|PWT(UC)
    ; ============================================================
    ; ---- PML4[0] = pdpt | 0x03 ----
    mov eax, pdpt
    or  eax, 0x03
    mov dword [pml4], eax
    mov dword [pml4 + 4], 0

    ; ---- PDPT[0] = pd_low | 0x03 ----
    mov eax, pd_low
    or  eax, 0x03
    mov dword [pdpt], eax
    mov dword [pdpt + 4], 0

    ; ---- PDPT[3] = pd_hi | 0x03  (偏移 3*8=24) 覆盖 0xC0000000..0xFFFFFFFF ----
    mov eax, pd_hi
    or  eax, 0x03
    mov dword [pdpt + 24], eax
    mov dword [pdpt + 28], 0

    ; ---- pd_low[i] = i*2MB | 0x83, i=0..15 (32MB) ----
    xor ecx, ecx
    xor eax, eax
.L1:
    mov edx, ecx
    shl edx, 21                 ; i * 2MB
    or  edx, 0x83
    mov [pd_low + ecx * 8], edx
    mov dword [pd_low + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 16
    jb  .L1

    ; pd_hi 索引 = (phys >> 21) & 0x1FF  (PDPT[3] 基址 0xC0000000)
    ; ---- 映射整个 0xC0000000..0xFFFFFFFF (1GB MMIO 区, UC): 512 个 2MB 页.
    ;      QEMU 的 LFB 实际 BAR 可能是 0xE0000000 或 0xFD000000, 全部覆盖. ----
    xor ecx, ecx
.L2:
    mov edx, ecx
    shl edx, 21                 ; i * 2MB
    add edx, 0xC0000000         ; 注意: 结果在 32 位内回绕, 高位恒 0
    or  edx, 0x8F               ; Present|PS|PCD|PWT (uncacheable)
    mov [pd_hi + ecx * 8], edx
    mov dword [pd_hi + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb  .L2

    ; ---- CR4.PAE (bit5) ----
    mov eax, cr4
    or  eax, 0x20
    mov cr4, eax

    ; ---- CR3 = PML4 ----
    mov eax, pml4
    mov cr3, eax

    ; ---- EFER.LME (MSR 0xC0000080, bit8) ----
    mov ecx, 0C0000080h
    rdmsr
    or  eax, 0x100
    wrmsr

    ; ---- 载入含 64 位代码段的 GDT ----
    mov  eax, gdt64
    push dword eax
    push word GDT64_LIMIT
    o32 lgdt [esp]
    add  sp, 6

    ; ---- 开启分页 (CR0.PG), 保持 PE/ET/NE ----
    mov eax, cr0
    or  eax, 80010000h          ; PG | PE
    mov cr0, eax

    jmp dword 0x18:long_start         ; 远跳进 64 位代码段

; ----------------------------------------------------------------------------
BITS 64
long_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    xor rbp, rbp
    and rsp, -16

    call kmain

.hang:
    cli
    hlt
    jmp .hang

; ----------------------------------------------------------------------------
section .data
align 8
GDT32_LIMIT equ 23
gdt32:
    dq 0x0000000000000000           ; 0x00 空
    dq 0x00CF9A000000FFFF           ; 0x08 32 位代码
    dq 0x00CF92000000FFFF           ; 0x10 数据
gdt32_end:

align 8
GDT64_LIMIT equ 31
gdt64:
    dq 0x0000000000000000           ; 0x00 空
    dq 0x00CF9A000000FFFF           ; 0x08 32 位代码 (保留)
    dq 0x00CF92000000FFFF           ; 0x10 数据
    dq 0x00A09A0000000000           ; 0x18 64 位代码: byte5=9A(access), byte6=A0(G=1,L=1)
gdt64_end:

; ----------------------------------------------------------------------------
section .bss
align 4096
pml4:   resb 4096
pdpt:   resb 4096
pd_low: resb 4096
pd_hi:  resb 4096
align 16
stack_bottom:
    resb 32768                      ; 32 KiB 内核栈
stack_top:
