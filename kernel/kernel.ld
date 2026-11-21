/* GT-DOS 内核链接脚本: 内核被引导扇区加载到 0x10000 */
ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    /* 实模式入口必须位于镜像最前面 */
    .boot16 : { *(.boot16) }

    . = ALIGN(16);
    .text : { *(.text) *(.text.*) }

    . = ALIGN(16);
    .rodata : { *(.rodata) *(.rodata.*) }

    /* 只读段结束边界: 4KB 对齐, 入口代码据此把 [0x10000, __ro_end) 设为只读 */
    . = ALIGN(4096);
    __ro_end = .;
    .romark : { KEEP(*(.romark)) }

    . = ALIGN(16);
    .data : { *(.data) *(.data.*) }

    /* 内核容器尾部 (protect.c): 必须位于 .bss 之前, 否则入口清零会抹掉它 */
    . = ALIGN(16);
    .container : { KEEP(*(.container)) }

    __bss_start = .;
    .bss : {
        *(.bss) *(.bss.*)
        *(COMMON)
    }
    __bss_end = .;

    /* 大缓冲 (字模/码点表) 放到 1MB 以上, 避免 BSS 覆盖 0xA0000 显存孔径 */
    . = 0x100000;
    .highbss (NOLOAD) : {
        *(.highbss) *(.highbss.*)
    }
    __highbss_end = .;

    /* 安全边界: .bss 不得越过 0xA0000 (VGA 孔径), 大缓冲不得撞上 LIB_BASE */
    ASSERT(__bss_end <= 0xA0000,
           "kernel .bss overlaps the VGA aperture; move buffers to .highbss")
    ASSERT(__highbss_end <= 0x580000,
           "kernel .highbss overlaps LIB_BASE")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.debug*)
    }
}