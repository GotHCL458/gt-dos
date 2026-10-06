/* ==========================================================================
 *  GT-DOS IDT / 异常处理 / 中断控制器 (8259A PIC 或 IO-APIC)
 *  ---------------------------------------------------------------------------
 *  - 中断嵌套: handler 执行期间开中断 (sti), EOI 延后到 handler 之后.
 *      PIC 模式: 8259A 的 in-service 位天然阻止同级/低级 IRQ, 高级可打断.
 *      APIC 模式: 用 TPR 屏蔽同级/低级, 高级可打断.
 *  - 向量布局: 0..31 CPU 异常; 32..47 PIC IRQ; 64..79 APIC IRQ; 255 杂散.
 * ========================================================================== */
#include "gt.h"

#define IDT_ENTRIES 256
#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI 0x20

#define APIC_VECTOR_BASE 64           /* IRQ n -> 向量 64+n (APIC 模式) */
#define SPURIOUS_VECTOR  255

struct idt_entry {
    u16 base_lo;
    u16 sel;
    u8  ist;              /* bits 0-2 = IST, rest 0 */
    u8  flags;
    u16 base_mid;
    u32 base_hi;
    u32 zero;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;
static irq_handler_t    irq_handlers[16];

/* ---------------------------------------------------------- 嵌套状态 */
static volatile int in_irq;           /* 当前正在服务的中断层数 */
static int nest_max;                  /* 历史最大嵌套深度 */
static volatile int apic_mode;        /* 0=8259A, 1=IO-APIC+本地APIC */

/* IRQ 优先级说明:
 *  - PIC 模式: 8259A 硬件优先级固定 (IR0 最高), 嵌套由 in-service 位决定,
 *    即 timer(IRQ0) 可打断一切, kbd(IRQ1) 可打断 IRQ2..15, 以此类推.
 *  - APIC 模式: 优先级由向量高 4 位决定 (见 apic.c irq_vec), 处理 IRQ 时
 *    设 TPR=向量&0xF0, 只允许更高向量类的中断打断. */

int irq_in_service(void) { return in_irq; }
int irq_nest_max(void)   { return nest_max; }
int irq_using_apic(void) { return apic_mode; }
void irq_set_apic_mode(int on) { apic_mode = on; }

/* apic.c 提供 */
extern void apic_eoi(int irq);
extern void apic_set_tpr(u8 prio);

/* isr.asm 中的桩 */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void airq0(void);  extern void airq1(void);  extern void airq2(void);
extern void airq3(void);  extern void airq4(void);  extern void airq5(void);
extern void airq6(void);  extern void airq7(void);  extern void airq8(void);
extern void airq9(void);  extern void airq10(void); extern void airq11(void);
extern void airq12(void); extern void airq13(void); extern void airq14(void);
extern void airq15(void);
extern void isrspl(void);               /* 杂散中断向量 255 */

static const char *exception_names[32] = {
    "Divide-by-zero",          "Debug",              "Non-maskable Interrupt",
    "Breakpoint",              "Overflow",           "Bound Range Exceeded",
    "Invalid Opcode",          "Device Not Available",
    "Double Fault",            "Coprocessor Segment Overrun",
    "Invalid TSS",             "Segment Not Present",
    "Stack-Segment Fault",     "General Protection Fault",
    "Page Fault",              "Reserved",
    "x87 FP Exception",        "Alignment Check",
    "Machine Check",           "SIMD FP Exception",
    "Virtualization Exception","Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor Injection",
    "VMM Communication", "Security Exception", "Reserved"
};

static void idt_set_gate(u8 num, u64 base, u16 sel, u8 flags)
{
    idt[num].base_lo = (u16)(base & 0xFFFF);
    idt[num].base_mid = (u16)((base >> 16) & 0xFFFF);
    idt[num].base_hi = (u32)(base >> 32);
    idt[num].sel     = sel;
    idt[num].ist     = 0;
    idt[num].flags   = flags;
    idt[num].zero    = 0;
}

/* 供 apic.c 使用: 把桩装到指定向量 (APIC 优先级向量布局) */
void idt_set_apic_gate(u8 vec, u64 base)
{
    idt_set_gate(vec, base, 0x18, 0x8E);
}

void pic_init(void)
{
    /* 把 IRQ0..15 重映射到 32..47, 避开 CPU 异常 */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0x00);  /* 全部解除屏蔽 */
    outb(PIC2_DATA, 0x00);
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void irq_install_handler(int irq, irq_handler_t fn)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = fn;
}

void irq_uninstall_handler(int irq)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = 0;
}

void panic(const char *msg)
{
    cpu_cli();
    kprintf("\n\n*** GT-DOS KERNEL PANIC ***\n%s\n", msg);
    kprintf("System halted.\n");
    for (;;)
        cpu_halt();
}

void isr_handler(struct regs *r)
{
    if (r->int_no == SPURIOUS_VECTOR)
        return;                       /* 杂散中断: 直接返回, 无需 EOI */

    if (r->int_no < 32) {
        cpu_cli();
        kprintf("\n\n*** GT-DOS EXCEPTION %u: %s ***\n",
                (u32)r->int_no, exception_names[r->int_no]);
        if (r->int_no == 14)
            kprintf("CR2 (fault address) = %p\n", (void *)(u64)gt_read_cr2());
        kprintf("err=%x  rip=%p  cs=%x  rflags=%x\n",
                (u32)r->err_code, (void *)r->rip, (u32)r->cs, (u32)r->rflags);
        kprintf("rax=%p rbx=%p rcx=%p rdx=%p\n",
                (void *)r->rax, (void *)r->rbx, (void *)r->rcx, (void *)r->rdx);
        kprintf("rsi=%p rdi=%p rbp=%p rsp=%p\n",
                (void *)r->rsi, (void *)r->rdi, (void *)r->rbp, (void *)r->rsp);
        kprintf("r8=%p r9=%p r10=%p r11=%p\n",
                (void *)r->r8, (void *)r->r9, (void *)r->r10, (void *)r->r11);
        kprintf("r12=%p r13=%p r14=%p r15=%p\n",
                (void *)r->r12, (void *)r->r13, (void *)r->r14, (void *)r->r15);
        kprintf("System halted.\n");
        for (;;)
            cpu_halt();
    }

    /* 硬件中断: PIC 模式向量 32..47; APIC 模式向量按优先级类分配 (查表).
     * 未知向量 (如被屏蔽的级联线 0x20 重放): 只发 EOI 吸收, 绝不调用 handler */
    int irq = -1;
    if (apic_mode) {
        irq = apic_vector_to_irq(r->int_no);
        if (irq < 0) {
            /* 残留 PIC 向量 (切换前入队的): 发 PIC EOI 吸收 */
            if (r->int_no >= 32 && r->int_no < 48)
                pic_send_eoi((int)r->int_no - 32);
            else if (r->int_no >= 32)
                apic_eoi(0);
            return;
        }
    } else if (r->int_no >= 32 && r->int_no < 48) {
        irq = (int)r->int_no - 32;
    }
    if (irq < 0)
        return;

    in_irq++;
    if (in_irq > nest_max)
        nest_max = in_irq;

    /* 打开中断窗口: 只有更高优先级的 IRQ 能打断我们.
     * PIC 靠 in-service 位天然过滤; APIC 用 TPR (向量&0xF0) 过滤. */
    if (apic_mode)
        apic_set_tpr((u8)(r->int_no & 0xF0));
    cpu_sti();

    if (irq_handlers[irq])
        irq_handlers[irq](r);

    cpu_cli();
    if (apic_mode)
        apic_set_tpr(0);

    /* EOI 放在 handler 之后: 处理期间 in-service/remote-IRR 保持置位,
     * 保证同级/低级中断不会插队 */
    if (apic_mode)
        apic_eoi(irq);
    else
        pic_send_eoi(irq);

    in_irq--;
}

void idt_init(void)
{
    idtr.limit = (u16)(sizeof(idt) - 1);
    idtr.base  = (u64)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate((u8)i, 0, 0, 0);

    for (int i = 0; i < 16; i++)
        irq_handlers[i] = 0;
    in_irq = 0;
    nest_max = 0;

    void (*stubs[32])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (int i = 0; i < 32; i++)
        idt_set_gate((u8)i, (u64)stubs[i], 0x18, 0x8E);

    void (*irqs[16])(void) = {
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
    };
    void (*airqs[16])(void) = {
        airq0,  airq1,  airq2,  airq3,  airq4,  airq5,  airq6,  airq7,
        airq8,  airq9,  airq10, airq11, airq12, airq13, airq14, airq15
    };
    for (int i = 0; i < 16; i++) {
        idt_set_gate((u8)(32 + i), (u64)irqs[i], 0x18, 0x8E);
        idt_set_gate((u8)(APIC_VECTOR_BASE + i), (u64)airqs[i], 0x18, 0x8E);
    }
    idt_set_gate(SPURIOUS_VECTOR, (u64)isrspl, 0x18, 0x8E);

    __asm__ __volatile__("lidt %0" ::"m"(idtr));
}
