; GT-DOS 示例共享库 (.gtl)
; 格式: "GTL1" + u32 version + u32 image_size + u32 export_count
;       + exports{ char name[24]; u32 rva; } + image
BITS 64

db "GTL1"
dd 1
dd image_end - image_start
dd 2

db "add", 0
times 20 db 0
dd add_fn - image_start

db "mul", 0
times 20 db 0
dd mul_fn - image_start

image_start:
add_fn:
    mov rax, rdi
    add rax, rsi
    ret

mul_fn:
    mov rax, rdi
    imul rax, rsi
    ret
image_end:
