/* ==========================================================================
 *  GT-DOS 8253/8254 PIT 定时器 + CMOS RTC 读取
 * ========================================================================== */
#include "gt.h"

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_FREQ 1193182u

static volatile u32 ticks;
static u32 tick_hz = 100;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
}

void timer_init(u32 hz)
{
    if (hz == 0) hz = 100;
    tick_hz = hz;
    ticks = 0;

    u32 divisor = PIT_FREQ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    outb(PIT_CMD, 0x36);                     /* 通道0, 先低后高, 方式3, 二进制 */
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));

    irq_install_handler(0, timer_irq);
}

u32 timer_ticks(void) { return ticks; }

u32 timer_uptime_ms(void)
{
    return ticks * (1000u / tick_hz);
}

void timer_sleep_ms(u32 ms)
{
    u32 start = timer_uptime_ms();
    while (timer_uptime_ms() - start < ms)
        cpu_halt();
}

/* ------------------------------------------------------------------- RTC */
static u8 cmos_read(u8 reg)
{
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static u8 bcd_to_bin(u8 v)
{
    return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

static bool rtc_updating(void)
{
    return (cmos_read(0x0A) & 0x80) != 0;
}

void rtc_read(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    while (rtc_updating())
        ;

    u8 s  = cmos_read(0x00);
    u8 m  = cmos_read(0x02);
    u8 h  = cmos_read(0x04);
    u8 wd = cmos_read(0x06);
    u8 d  = cmos_read(0x07);
    u8 mo = cmos_read(0x08);
    u8 y  = cmos_read(0x09);
    (void)wd;

    u8 regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {                    /* 非二进制 => BCD */
        s  = bcd_to_bin(s);
        m  = bcd_to_bin(m);
        d  = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y  = bcd_to_bin(y);
        bool pm = (h & 0x80) != 0;
        h = bcd_to_bin((u8)(h & 0x7F));
        if (!(regb & 0x02)) {                /* 12 小时制 */
            h = (u8)(h % 12);
            if (pm) h = (u8)(h + 12);
        }
    }

    if (year)  *year  = 2000 + y;
    if (month) *month = mo;
    if (day)   *day   = d;
    if (hour)  *hour  = h;
    if (min)   *min   = m;
    if (sec)   *sec   = s;
}