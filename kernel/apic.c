/* ==========================================================================
 *  GT-DOS 可选 APIC 模式 (IO-APIC + 本地 APIC, MMIO 访问)
 *  ---------------------------------------------------------------------------
 *  目的: 突破 8259A 的 16 中断上限, 为将来多核 SMP 铺路.
 *  本内核无分页, 直接恒等映射访问 0xFEE00000 (本地 APIC) 与
 *  0xFEC00000 (IO-APIC). 仅在单 CPU BSP 上运行, 用 MMIO 而非 x2APIC MSR.
 *
 *  切换步骤 (apic_enable):
 *    1) 本地 APIC: 设 LDR/DFR, 开 spurious (向量 255), 屏蔽 LVT0/1 (ExtINT);
 *    2) IO-APIC: ISA IRQ0..15 -> 重定向表项 0..15, 向量 64+n, 电平触发;
 *    3) 屏蔽 8259A (IMR=0xFF), 此后中断只经 APIC 进来.
 *  回退 (apic_disable): 反向恢复 PIC.
 * ========================================================================== */
#include "gt.h"

#define LAPIC_BASE 0xFEE00000u
#define IOAPIC_ID  0xFEC00000u
#define IOAPIC_SEL (IOAPIC_ID + 0x00)
#define IOAPIC_WIN (IOAPIC_ID + 0x10)

/* 本地 APIC 寄存器偏移 */
#define LAPIC_ID     0x020
#define LAPIC_VER    0x030
#define LAPIC_TPR    0x080
#define LAPIC_EOI    0x0B0
#define LAPIC_SVR    0x0F0
#define LAPIC_DFR    0x0E0
#define LAPIC_LDR    0x0D0
#define LAPIC_LVT0   0x130
#define LAPIC_LVT1   0x140
#define LAPIC_SPVT   0x150

#define APIC_VECTOR_BASE 64
#define APIC_ENABLED     0x100
#define APIC_FIXED       0x000          /* delivery mode */
#define APIC_MASKED      0x10000

/* IRQ -> 向量映射: 向量高 4 位即优先级类 (越大越紧急).
 * 紧急度: timer > kbd/mouse > disk > com > 其余.
 * 处理 IRQ n 时设 TPR = 向量&0xF0, 只允许更高向量类的中断打断 (嵌套). */
static const u8 irq_vec[16] = {
    144, /* 0  timer  (类9, 最紧急) */
    128, /* 1  kbd    (类8) */
    64,  /* 2  cascade(类4) */
    65,  /* 3  com2   (类4) */
    80,  /* 4  com1   (类5) */
    66,  /* 5  lpt2   (类4) */
    81,  /* 6  floppy (类5) */
    67,  /* 7  lpt1   (类4) */
    68,  /* 8  rtc    (类4) */
    69,  /* 9         (类4) */
    96,  /* 10        (类6) */
    70,  /* 11        (类4) */
    129, /* 12 mouse  (类8) */
    71,  /* 13 fpu    (类4) */
    112, /* 14 ata0   (类7) */
    113, /* 15 ata1   (类7) */
};

u8 apic_irq_vector(int irq)
{
    return (irq >= 0 && irq < 16) ? irq_vec[irq] : 0;
}

int apic_vector_to_irq(u32 vec)
{
    for (int i = 0; i < 16; i++)
        if (irq_vec[i] == (u8)vec)
            return i;
    return -1;
}

static volatile u32 *lapic;
static bool apic_ok;                    /* 硬件可用 */
static bool apic_on;                    /* 当前已启用 */

/* airqN 桩 (isr.asm), 各自 push irq_vec[N] 对应向量 */
extern void airq0(void);   extern void airq1(void);   extern void airq2(void);
extern void airq3(void);   extern void airq4(void);   extern void airq5(void);
extern void airq6(void);   extern void airq7(void);   extern void airq8(void);
extern void airq9(void);   extern void airq10(void);  extern void airq11(void);
extern void airq12(void);  extern void airq13(void);  extern void airq14(void);
extern void airq15(void);

/* 把 airq 桩装到各自向量号上 (IDT 门) */
static void apic_install_idt(void)
{
    void (*f[16])(void) = {
        airq0,  airq1,  airq2,  airq3,  airq4,  airq5,  airq6,  airq7,
        airq8,  airq9,  airq10, airq11, airq12, airq13, airq14, airq15
    };
    for (int i = 0; i < 16; i++)
        idt_set_apic_gate(irq_vec[i], (u64)f[i]);
}

static inline u32 lapic_read(u32 reg)
{
    return lapic[reg / 4];
}
static inline void lapic_write(u32 reg, u32 val)
{
    lapic[reg / 4] = val;
    (void)lapic[0x30 / 4];              /* 读回, 确保写完成 */
}

static u32 ioapic_read(u32 reg)
{
    *(volatile u32 *)IOAPIC_SEL = reg;
    return *(volatile u32 *)IOAPIC_WIN;
}
static void ioapic_write(u32 reg, u32 val)
{
    *(volatile u32 *)IOAPIC_SEL = reg;
    *(volatile u32 *)IOAPIC_WIN = val;
}

/* CPUID.1:EDX bit9 = APIC present */
static bool cpu_has_apic(void)
{
    u32 a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (d & (1u << 9)) != 0;
}

bool apic_available(void)
{
    if (apic_ok)
        return true;
    if (!cpu_has_apic())
        return false;
    lapic = (volatile u32 *)LAPIC_BASE;
    /* 版本寄存器可读且非全 1/全 0 视为存在 */
    u32 ver = lapic_read(LAPIC_VER);
    if (ver == 0 || ver == 0xFFFFFFFFu)
        return false;
    apic_ok = true;
    return true;
}

/* 把 ISA IRQ 路由到 IO-APIC 重定向表项 (向量按优先级类分配).
 * 注意标准 PC 接线: ISA IRQ0(timer) 接 IO-APIC pin2, 其余 IRQn 接 pin n;
 * pin0/pin1 中 pin1=键盘. 用 pin_of[] 表处理这个历史遗留错位. */
static int ioapic_pin_of(int irq)
{
    return irq == 0 ? 2 : irq;
}

static void ioapic_route(int irq, bool mask)
{
    int pin = ioapic_pin_of(irq);
    u32 lo = (u32)irq_vec[irq];
    if (mask)
        lo |= APIC_MASKED;
    else
        lo |= APIC_FIXED;               /* delivery fixed */
    lo |= (1u << 15);                   /* level trigger (ISA 默认) */
    ioapic_write(0x10 + pin * 2, lo);
    ioapic_write(0x11 + pin * 2, 0);    /* destination = LAPIC id 0 */
}

bool apic_enable(void)
{
    if (!apic_available())
        return false;
    if (apic_on)
        return true;

    apic_install_idt();               /* 先把桩装到优先级向量上 */

    /* 本地 APIC: flat 模式, 逻辑 id 0, 开 spurious (向量 255) */
    lapic_write(LAPIC_DFR, 0xF0000000u);
    lapic_write(LAPIC_LDR, 0x00000000u);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT0, APIC_MASKED);   /* 8259 已屏蔽, 不走 ExtINT */
    lapic_write(LAPIC_LVT1, APIC_MASKED);
    lapic_write(LAPIC_SPVT, APIC_MASKED);
    lapic_write(LAPIC_SVR, APIC_ENABLED | 255);

    /* IO-APIC: 屏蔽全部引脚, 再打开 IRQ0..15.
     * 引脚数在"版本寄存器"(索引 0x01) 的 bits 23:16, 不是 ID 寄存器! */
    u32 ver = ioapic_read(0x01);
    int pins = (int)(((ver >> 16) & 0xFF) + 1);
    if (pins > 24) pins = 24;
    for (int i = 0; i < pins; i++) {
        ioapic_write(0x10 + i * 2, APIC_MASKED | (1u << 15));
        ioapic_write(0x11 + i * 2, 0);
    }
    for (int i = 0; i < 16 && i < pins; i++) {
        if (i == 2) continue;           /* ISA IRQ2 级联线: APIC 模式下
                                           slave PIC 已废弃, 保持屏蔽,
                                           否则电平悬空导致中断风暴 */
        ioapic_route(i, false);
    }

    /* 屏蔽 8259A, 避免双路中断 */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    apic_on = true;
    irq_set_apic_mode(1);
    return true;
}

void apic_disable(void)
{
    if (!apic_on)
        return;
    irq_set_apic_mode(0);
    lapic_write(LAPIC_SVR, 0);          /* 关本地 APIC */
    for (int i = 0; i < 16; i++)
        ioapic_route(i, true);
    apic_on = false;
    /* PIC 由调用方重新 pic_init() */
}

bool apic_active(void) { return apic_on; }

/* EOI: 写本地 APIC EOI 寄存器 (优先级由 ISR 自动恢复) */
void apic_eoi(int irq)
{
    (void)irq;
    if (apic_on)
        lapic_write(LAPIC_EOI, 0);
}

/* 写 TPR (原始值, 通常 = 当前向量 & 0xF0): 屏蔽同级/低级中断 */
void apic_set_tpr(u8 raw)
{
    if (apic_on)
        lapic_write(LAPIC_TPR, raw);
}
