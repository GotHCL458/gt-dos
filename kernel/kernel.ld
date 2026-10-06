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

    . = ALIGN(16);
    .data : { *(.data) *(.data.*) }

    . = ALIGN(16);
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

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.debug*)
    }
}