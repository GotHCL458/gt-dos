; ============================================================================
;  GT-DOS  Stage 0: MBR (主引导记录, LBA0)
;  ---------------------------------------------------------------------------
;  职责: 从磁盘高端隐藏启动分区读取 stage2 (16 扇区) 到 0x7E00 并跳转.
;        使用 INT 13h AH=42h (LBA DAP), 不依赖 CHS.
;  分区表由 build.py 直接写入 (本文件只保留 0x1BE..0x1FE 空间).
;  汇编: nasm -f bin boot/mbr.asm -o build/mbr.bin
; ============================================================================

[BITS 16]
[ORG 0x7C00]

BOOT_DRIVE_ADDR equ 0x0600
STAGE2_SEG      equ 0x0000
STAGE2_OFF      equ 0x7E00
STAGE2_COUNT    equ 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    mov [BOOT_DRIVE_ADDR], dl

    mov si, msg_mbr
    call puts

    ; ---- 复位磁盘 ----
    xor ah, ah
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13

    ; ---- AH=42h 读 stage2 ----
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    mov si, dap
    int 0x13
    jnc .ok
    ; 失败重试一次
    xor ah, ah
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    mov si, dap
    int 0x13
    jc .fail

.ok:
    mov si, msg_jump
    call puts
    jmp STAGE2_SEG:STAGE2_OFF

.fail:
    mov si, msg_err
    call puts
.hang:
    hlt
    jmp .hang

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
msg_mbr  db 13, 10, 'GT-DOS MBR', 0
msg_jump db ' -> stage2', 13, 10, 0
msg_err  db 13, 10, 'GT-DOS: stage2 read error!', 13, 10, 0

; Disk Address Packet (16 字节). 'GTD0' 标记末尾 +8 = LBA low (build 填)
db 'GTD0'
dap:
    db 16, 0
    dw STAGE2_COUNT
    dw STAGE2_OFF
    dw STAGE2_SEG
    dd 0                  ; LBA low  (build.py 填充启动分区起始 LBA)
    dd 0                  ; LBA high

    times 0x1BE-($-$$) db 0   ; 引导代码填充
    ; ---- 0x1BE: 分区表 (build.py 写入) ----
    times 0x1FE-($-$$) db 0
    dw 0xAA55
