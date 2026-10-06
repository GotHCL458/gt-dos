; ============================================================================
;  GT-DOS  Stage 2 引导器 (由 MBR 加载到 0x7E00)
;  ---------------------------------------------------------------------------
;  职责: 从高端隐藏启动分区用 INT13h AH=42h (LBA DAP) 把内核加载到 0x10000,
;        并把 font.bin / cpm.bin 的起始 LBA 与扇区数写入内存邮箱, 由内核在
;        保护模式 (A20 已开) 用 ATA 读取. 最后跳转内核入口 (实模式).
;
;  各文件的起始 LBA 与扇区数由 build.py 通过标记补丁写入.
;  汇编: nasm -f bin boot/boot.asm -o build/boot.bin
; ============================================================================

[BITS 16]
[ORG 0x7E00]

; ---- 传给内核的邮箱 ----
BOOT_DRIVE_ADDR equ 0x0600          ; 引导盘号
FONT_LBA_ADDR   equ 0x0604          ; u32 font.bin 起始 LBA
FONT_SECT_ADDR  equ 0x0608          ; u32 font.bin 扇区数
CPM_LBA_ADDR    equ 0x060C          ; u32 cpm.bin 起始 LBA
CPM_SECT_ADDR   equ 0x0610          ; u32 cpm.bin 扇区数

KERNEL_SEG equ 0x1000               ; 0x10000
MAX_CHUNK  equ 32                   ; 每次 INT13 最多读 32 扇区 (16KB)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    mov [BOOT_DRIVE_ADDR], dl

    mov si, msg_s2
    call puts

    ; ---- 读 kernel (0x10000, 低端内存, 无需 A20) ----
    mov eax, [kernel_start_lba]
    mov [cur_lba], eax
    mov ax, [kernel_sector_count]
    mov [cur_cnt], ax
    mov word [cur_seg], KERNEL_SEG
    mov word [cur_off], 0
    call load_file
    jc  .fail

    ; ---- 填邮箱: font/cpm 的位置 (内核自行读取) ----
    mov eax, [font_start_lba]
    mov [FONT_LBA_ADDR], eax
    mov eax, [font_sector_count]
    mov [FONT_SECT_ADDR], eax
    mov eax, [cpm_start_lba]
    mov [CPM_LBA_ADDR], eax
    mov eax, [cpm_sector_count]
    mov [CPM_SECT_ADDR], eax

    mov si, msg_ok
    call puts

    mov dl, [BOOT_DRIVE_ADDR]
    jmp KERNEL_SEG:0x0000

.fail:
    mov si, msg_err
    call puts
.hang:
    hlt
    jmp .hang

; ----------------------------------------------------------------------------
; load_file: 按 cur_lba/cur_cnt/cur_seg:cur_off 分块读完
;   失败返回 CF=1
load_file:
    cmp word [cur_cnt], 0
    jz  .done
    call read_chunk
    jc  .fail
    jmp load_file
.done:
    clc
    ret
.fail:
    stc
    ret

; ----------------------------------------------------------------------------
; read_chunk: 读一个扇区, 推进 cur_lba/cur_cnt/cur_off (每扇区一次, 最可靠)
read_chunk:
    push ax
    push cx
    push dx
    push si

    mov si, dap
    mov byte [si], 16
    mov byte [si+1], 0
    mov word [si+2], 1              ; 1 扇区
    mov ax, [cur_off]
    mov word [si+4], ax
    mov ax, [cur_seg]
    mov word [si+6], ax
    mov eax, [cur_lba]
    mov dword [si+8], eax
    mov dword [si+12], 0

    mov word [retry], 4
.retry:
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jnc .ok
    dec word [retry]
    jz  .bad
    xor ah, ah
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jmp .retry
.ok:
    inc dword [cur_lba]
    dec word [cur_cnt]
    add word [cur_off], 512
    jnc .nocarry
    add word [cur_seg], 0x1000
.nocarry:
    clc
    pop si
    pop dx
    pop cx
    pop ax
    ret
.bad:
    stc
    pop si
    pop dx
    pop cx
    pop ax
    ret

; ---------------------------------------------------------------- put string
puts:
    push ax
    push bx
.next:
    lodsb
    test al, al
    jz  .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .next
.done:
    pop bx
    pop ax
    ret

; ---------------------------------------------------------------- 数据区
msg_s2 db 13, 10, 'GT-DOS stage2: loading kernel...', 13, 10, 0
msg_ok db 'GT-DOS: entering kernel...', 13, 10, 0
msg_err db 'GT-DOS: LBA read error!', 13, 10, 0

dap     times 16 db 0
cur_lba dd 0
cur_cnt dw 0
cur_seg dw 0
cur_off dw 0
retry   dw 0

db 'GTCN'
kernel_sector_count: dd 0
db 'GTKL'
kernel_start_lba:    dd 0
db 'GTFL'
font_start_lba:      dd 0
db 'GTFS'
font_sector_count:   dd 0
db 'GTCL'
cpm_start_lba:       dd 0
db 'GTCS'
cpm_sector_count:    dd 0

    times 512*16-($-$$) db 0        ; stage2 占满 16 扇区
