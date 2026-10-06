/* ==========================================================================
 *  GT-DOS PS/2 鼠标驱动 (IRQ12) + IntelliMouse 滚轮检测
 *  ---------------------------------------------------------------------------
 *  关键点:
 *    1) 初始化时必须正确读写 8042 控制器的配置字节 (0x20 读 / 0x60 写):
 *       bit0=键盘中断使能, bit1=鼠标中断使能, bit4/5=禁止两端口.
 *       漏掉这一步会导致键盘或鼠标中断根本没开, QEMU 窗口里无法输入.
 *    2) 键盘与鼠标共用输出缓冲 0x60, 靠状态寄存器 bit5 区分数据来源.
 *       两边的 IRQ 处理都必须检查 bit5, 否则会互相吞字节导致失步.
 *
 *  滚轮检测: 采样率魔法序列 200 -> 100 -> 50 后读设备 ID,
 *  返回 3 即 IntelliMouse, 之后每包 4 字节, 第 4 字节低 4 位为滚轮量.
 * ========================================================================== */
#include "gt.h"

#define KB_DATA      0x60
#define KB_STATUS    0x64
#define KB_CMD       0x64

static bool present;                 /* 检测到可用鼠标 */
static bool wheel_mode;
static wheel_cb_t on_wheel;

static u8  pkt[4];
static int pkt_i;

/* 光标位置与按键状态 (GUI 用). 移动事件在 bottom-half 里更新. */
static int mx, my;                     /* 屏幕坐标 (0..639 / 0..479) */
static int mb;                         /* bit0=左 bit1=右 bit2=中 */
static volatile int m_moved;           /* 有新移动事件未消费 */
static int m_dx, m_dy;

/* 事件环 (GUI 消费): type=0 移动 / 1 按键 / 2 滚轮 */
struct mevt { u8 type; i16 a, b; u8 btn; };
#define MEVT_RING 64
static volatile struct mevt mevts[MEVT_RING];
static volatile int mev_head, mev_tail;

static void mevt_push(u8 type, int a, int b, u8 btn)
{
    int next = (mev_head + 1) % MEVT_RING;
    if (next == mev_tail)
        return;
    mevts[mev_head].type = type;
    mevts[mev_head].a = (i16)a;
    mevts[mev_head].b = (i16)b;
    mevts[mev_head].btn = btn;
    mev_head = next;
}

int mouse_pop_event(int *a, int *b, int *btn)
{
    if (mev_tail == mev_head)
        return -1;
    *a = mevts[mev_tail].a;
    *b = mevts[mev_tail].b;
    *btn = mevts[mev_tail].btn;
    int t = mevts[mev_tail].type;
    mev_tail = (mev_tail + 1) % MEVT_RING;
    return t;
}

int  mouse_x(void) { return mx; }
int  mouse_y(void) { return my; }
int  mouse_buttons(void) { return mb; }

/* 取出并清零累计位移; 返回 false 表示期间无移动 */
bool mouse_take_delta(int *dx, int *dy)
{
    if (!m_moved)
        return false;
    *dx = m_dx;
    *dy = m_dy;
    m_dx = m_dy = 0;
    m_moved = 0;
    return true;
}

void mouse_warp(int x, int y)
{
    mx = x;
    my = y;
}

/* ------------------------------------------------------------ 8042 原语 */
static bool wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(KB_STATUS) & 0x02))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(KB_STATUS) & 0x01)
            return true;
    return false;
}

static void ctrl_cmd(u8 cmd)
{
    if (wait_write())
        outb(KB_CMD, cmd);
}

static void ctrl_write(u8 val)
{
    if (wait_write())
        outb(KB_DATA, val);
}

static int ctrl_read(void)            /* 从 0x60 读一个响应字节 */
{
    if (!wait_read())
        return -1;
    return inb(KB_DATA);
}

static u8 read_config(void)
{
    ctrl_cmd(0x20);
    return (u8)ctrl_read();
}

static void write_config(u8 val)
{
    ctrl_cmd(0x60);
    ctrl_write(val);
}

/* 向鼠标发命令 (经控制器 0xD4 转发) 并等待 ACK (0xFA) */
static bool mouse_write(u8 val)
{
    ctrl_cmd(0xD4);
    ctrl_write(val);
    return ctrl_read() == 0xFA;
}

/* 读取鼠标设备 ID (命令 0xF2) */
static int mouse_read_id(void)
{
    if (!mouse_write(0xF2))
        return -1;
    return ctrl_read();
}

static bool mouse_set_rate(u8 rate)
{
    return mouse_write(0xF3) && mouse_write(rate);
}

/* 丢弃输出缓冲里所有滞留字节 (含鼠标复位/ACK 期间挤进来的键盘码) */
static void flush_input(void)
{
    int guard = 0;
    while ((inb(KB_STATUS) & 0x01) && guard++ < 64) {
        u8 b = (u8)inb(KB_DATA);
        if (inb(KB_STATUS) & 0x20)
            mouse_input_byte(b);          /* 滞留的鼠标字节照常处理 */
        else
            keyboard_input_byte(b);       /* 滞留的键盘字节照常处理 */
    }
}

/* ------------------------------------------------------------------ IRQ */
/* top-half: 只取字节入 raw 环 + raise; 组包与滚轮回调在 bottom-half 执行 */
static volatile u8 m_raw[64];
static volatile int m_head, m_tail;

void mouse_input_byte(u8 b)
{
    int len = wheel_mode ? 4 : 3;

    /* 首字节必须 bit3=1, 否则丢弃重新同步 */
    if (pkt_i == 0 && !(b & 0x08))
        return;

    pkt[pkt_i++] = b;
    if (pkt_i < len)
        return;

    pkt_i = 0;

    /* 更新光标位置与按键 (bottom-half 上下文, GUI 消费)
     * byte1/byte2 本身就是二进制补码有符号数, 符号位 (bit4/5) 与之冗余,
     * 不可再次取反, 否则向下/向左移动会被翻转成向上/向右. */
    {
        int dx = (int)(signed char)pkt[1];
        int dy = (int)(signed char)pkt[2];
        int nbtn = pkt[0] & 0x07;
        mx += dx;
        my -= dy;                       /* PS/2 dy 向上为正, 屏幕 y 向下 */
        int wmax = gfx_active() ? gfx_w() : 640;
        int hmax = gfx_active() ? gfx_h() : 480;
        if (mx < 0) mx = 0;
        if (mx > wmax - 1) mx = wmax - 1;
        if (my < 0) my = 0;
        if (my > hmax - 1) my = hmax - 1;
        m_dx += dx;
        m_dy -= dy;
        if (dx || dy) {
            m_moved = 1;
            mevt_push(0, mx, my, (u8)nbtn);
        }
        if (nbtn != mb) {
            mb = nbtn;
            mevt_push(1, mx, my, (u8)mb);
        }
    }

    if (wheel_mode) {
        /* 第 4 字节低 4 位是滚轮量, 4 位二进制补码:
         *   1..7  -> 向上滚 (正值)
         *   8..15 -> 向下滚 (-8..-1) */
        int w = pkt[3] & 0x0F;
        if (w >= 8)
            w -= 16;
        if (w != 0) {
            mevt_push(2, w, 0, (u8)mb);
            if (on_wheel && !gui_active())   /* GUI 桌面里滚轮归窗口, 不滚 shell */
                on_wheel(w);
        }
    }
    /* 移动/按键暂不处理: 文本控制台无图形指针 */
}

static void mouse_bottom_half(void)
{
    while (m_tail != m_head) {
        u8 b = m_raw[m_tail];
        m_tail = (m_tail + 1) % 64;
        mouse_input_byte(b);
    }
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    /* 状态寄存器 bit5=1 表示数据来自鼠标; 否则其实是键盘字节, 交还键盘 */
    if (!(inb(KB_STATUS) & 0x20)) {
        keyboard_input_byte((u8)inb(KB_DATA));
        return;
    }
    u8 b = (u8)inb(KB_DATA);
    int next = (m_head + 1) % 64;
    if (next != m_tail) {
        m_raw[m_head] = b;
        m_head = next;
    }
    softirq_raise(SOFTIRQ_MOUSE);
}

/* ------------------------------------------------------------------ 初始化 */
void mouse_init(wheel_cb_t cb)
{
    on_wheel = cb;
    present = false;
    wheel_mode = false;
    pkt_i = 0;

    flush_input();

    /* 1) 正确配置 8042: 使能键盘+鼠标中断, 使能两端口传输 */
    ctrl_cmd(0xAD);                   /* 关键盘口 */
    ctrl_cmd(0xA7);                   /* 关鼠标口 */
    flush_input();

    u8 cfg = read_config();
    cfg |= 0x01;                      /* bit0: 键盘中断使能 */
    cfg |= 0x02;                      /* bit1: 鼠标中断使能 */
    cfg &= (u8)~0x10;                 /* bit4: 键盘口传输使能 */
    cfg &= (u8)~0x20;                 /* bit5: 鼠标口传输使能 */
    write_config(cfg);

    ctrl_cmd(0xAE);                   /* 开键盘口 */
    ctrl_cmd(0xA8);                   /* 开鼠标口 */
    flush_input();

    /* 2) 复位鼠标: 期待 0xAA 0x00 */
    if (!mouse_write(0xFF))
        goto fallback_no_mouse;
    ctrl_read();                      /* 0xAA */
    ctrl_read();                      /* 0x00 */
    flush_input();

    /* 3) IntelliMouse 魔法序列: 200 -> 100 -> 50 后读 ID == 0x03 */
    if (mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(50)) {
        if (mouse_read_id() == 0x03)
            wheel_mode = true;
        else
            mouse_set_rate(100);
    }
    flush_input();

    /* 4) 默认缩放/报告, 开启数据上报 */
    if (!mouse_write(0xE6)) goto fallback_no_mouse;
    if (!mouse_write(0xF4)) goto fallback_no_mouse;
    flush_input();

    present = true;
    m_head = m_tail = 0;
    softirq_set(SOFTIRQ_MOUSE, mouse_bottom_half);
    irq_install_handler(12, mouse_irq);
    return;

fallback_no_mouse:
    /* 鼠标不可用也要保证键盘中断已使能 (上面已写配置) */
    flush_input();
}

bool mouse_present(void) { return present; }