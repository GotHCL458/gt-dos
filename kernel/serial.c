/* ==========================================================================
 *  GT-DOS 串口驱动 (COM1)
 *  用途: 调试时把内核日志直接输出到宿主终端 (qemu -serial stdio)
 * ========================================================================== */
#include "gt.h"

#define COM1 COM1_BASE

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* 关闭中断 */
    outb(COM1 + 3, 0x80);   /* 允许设置波特率 (DLAB) */
    outb(COM1 + 0, 0x01);   /* 除数低字节: 115200 bps */
    outb(COM1 + 1, 0x00);   /* 除数高字节 */
    outb(COM1 + 3, 0x03);   /* 8 位数据, 无校验, 1 停止位 */
    outb(COM1 + 2, 0xC7);   /* 使能 FIFO, 清空, 14 字节阈值 */
    outb(COM1 + 4, 0x0B);   /* DTR + RTS + OUT2 */
}

static int serial_tx_ready(void)
{
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c)
{
    if (c == '\n') {
        serial_putc('\r');
    }
    while (!serial_tx_ready())
        ;
    outb(COM1, (u8)c);
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        serial_putc(s[i]);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

/* ---- 接收: 让终端也能当作键盘使用 ---- */
bool serial_has_data(void)
{
    return (inb(COM1 + 5) & 0x01) != 0;
}

int serial_getchar(void)
{
    if (!serial_has_data())
        return -1;
    return (int)inb(COM1);
}