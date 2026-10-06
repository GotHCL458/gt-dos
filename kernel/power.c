/* ==========================================================================
 *  GT-DOS 电源管理: 重启 / 关机
 *  ---------------------------------------------------------------------------
 *  关机按顺序尝试:
 *    1) PIIX4 ACPI 电源管理 (PCI 0:7.3 偏移 0x40 = PMBA; PM1_CNT = base+4,
 *       写 SLP_TYP=5|SLP_EN) —— QEMU 标准 ACPI 关机
 *    2) isa-debug-exit 设备 (端口 0xF4) —— QEMU 直接退出进程
 *    3) 全部失败则 hlt 死循环
 *  重启按顺序尝试:
 *    1) 8042 控制器 0xFE 命令 (拉低 CPU 复位线)
 *    2) 跳转向量 0xFFFF0 (BIOS 复位入口)
 * ========================================================================== */
#include "gt.h"

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

#define PCI_BDF(b, d, f, r) \
    (0x80000000u | ((u32)(b) << 16) | ((u32)(d) << 11) | ((u32)(f) << 8) | (r))

static u32 pci_r32(u32 addr)
{
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

static void pci_w32(u32 addr, u32 val)
{
    outl(PCI_CFG_ADDR, addr);
    outl(PCI_CFG_DATA, val);
}

void power_reboot(void)
{
    kprintf("Rebooting GT-DOS...\n");
    timer_sleep_ms(200);
    cpu_cli();

    /* 1) 8042: 命令 0xFE 拉低复位线 */
    outb(0x64, 0xFE);
    io_wait();

    /* 2) 快速 A20/复位门: 0x92 bit0 置 1 触发 CPU 软复位 */
    u8 v = inb(0x92);
    outb(0x92, (u8)(v | 0x01));
    io_wait();

    /* 3) 兜底: 制造 triple fault (加载非法 IDT 后 int3) -> CPU 复位 */
    struct { u16 limit; u64 base; } __attribute__((packed)) bad_idtr = { 0, 0 };
    __asm__ __volatile__("lidt %0\n\t" "int3" ::"m"(bad_idtr));

    for (;;)
        cpu_halt();
}

void power_shutdown(void)
{
    kprintf("It is now safe to shut down GT-DOS.\n");
    kprintf("Powering off...\n");
    timer_sleep_ms(200);
    cpu_cli();

    /* 1) PIIX4 ACPI PM: 把 PMBA 设到 0xB000 并使能, 再写 PM1_CNT */
    u32 pm = pci_r32(PCI_BDF(0, 7, 3, 0x40));
    u32 base = pm & 0xFFF0u;
    if (!(pm & 0x0001u)) {           /* 未使能 -> 强制写到 0xB000 */
        pci_w32(PCI_BDF(0, 7, 3, 0x40), 0x0000B001u);
        base = 0xB000u;
    }
    /* PM1_CNT: SLP_TYPa=5 (bit10-12), SLP_EN (bit13) */
    outl(base + 4, (5u << 10) | (1u << 13));
    io_wait();
    outl(base + 4, (5u << 10) | (1u << 13));   /* 再写一次保险 */

    /* 2) isa-debug-exit: 写 0xF4 -> QEMU 以 (val<<1)|1 退出进程 */
    outl(0xF4, 0x10);
    io_wait();
    outb(0xF4, 0x10);

    /* 3) 兜底: Bochs/QEMU 经典端口序列 */
    outw(0xB004, 0x2000 | (5 << 10));
    outw(0x402, 0x501);              /* QEMU x86 常见调试退出口 */

    for (;;)
        cpu_halt();
}