/* ==========================================================================
 *  GT-DOS PS/2 键盘驱动 (扫描码集 1, 含 E0 扩展键)
 *  ---------------------------------------------------------------------------
 *  普通键 -> 返回 ASCII
 *  扩展键 -> 返回 KEY_* 虚拟键码 (>= 0x100)
 * ========================================================================== */
#include "gt.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64
#define KB_CMD    0x64
#define KB_BUF_SIZE 256

/* 虚拟键码 */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103
#define KEY_HOME  0x104
#define KEY_END   0x105
#define KEY_DEL   0x106
#define KEY_INS   0x107
#define KEY_PGUP  0x108
#define KEY_PGDN  0x109

static u16 kb_buf[KB_BUF_SIZE];
static volatile int kb_head;
static volatile int kb_tail;

static bool shift_down;
static bool ctrl_down;
static bool alt_down;
static bool caps_lock;
static bool ext_pending;              /* 收到 E0, 下一字节是扩展码 */

static const char keymap_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char keymap_upper[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static void kb_push(u16 v)
{
    int next = (kb_head + 1) % KB_BUF_SIZE;
    if (next == kb_tail)
        return;
    kb_buf[kb_head] = v;
    kb_head = next;
}

static u16 ext_scancode(u8 sc)
{
    switch (sc) {
    case 0x48: return ctrl_down ? KEY_CUP   : KEY_UP;
    case 0x50: return ctrl_down ? KEY_CDOWN : KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x53: return KEY_DEL;
    case 0x52: return KEY_INS;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    default:   return 0;
    }
}

/* 处理一个来自 8042 输出缓冲的键盘字节 (扫描码集 1) */
void keyboard_input_byte(u8 sc)
{
    if (sc == 0xE0) { ext_pending = true; return; }

    if (ext_pending) {
        ext_pending = false;
        if (sc == 0x1D) { ctrl_down = true;  return; }   /* 右 Ctrl 按下 */
        if (sc == 0x9D) { ctrl_down = false; return; }   /* 右 Ctrl 松开 */
        if (sc == 0x38) { alt_down  = true;  return; }   /* 右 Alt 按下 */
        if (sc == 0xB8) { alt_down  = false; return; }   /* 右 Alt 松开 */
        if (sc & 0x80)
            return;                   /* 扩展键松开 */
        u16 v = ext_scancode(sc);
        if (v)
            kb_push(v);
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift_down = true;  return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = false; return; }
    if (sc == 0x1D)                { ctrl_down  = true;  return; }
    if (sc == 0x9D)                { ctrl_down  = false; return; }
    if (sc == 0x38)                { alt_down   = true;  return; }  /* 左 Alt */
    if (sc == 0xB8)                { alt_down   = false; return; }
    if (sc == 0x3A)                { caps_lock = !caps_lock; return; }

    if (sc & 0x80)
        return;                       /* 松键 */

    /* Ctrl+Alt+Del -> 重启组合键 */
    if (sc == 0x53 && ctrl_down && alt_down) {
        kb_push(KEY_REBOOT);
        return;
    }

    if (sc >= 128)
        return;

    char lo = keymap_lower[sc];
    char c;
    if (lo >= 'a' && lo <= 'z')
        c = (shift_down ^ caps_lock) ? keymap_upper[sc] : lo;
    else
        c = shift_down ? keymap_upper[sc] : lo;

    if (c)
        kb_push((u16)(unsigned char)c);
}

/* -------------------------------------------------- 上下半分离 (软中断) */
/* top-half: 只从端口取字节入 raw 环 + raise; 扫描码解码在 bottom-half */
static volatile u8 raw_buf[KB_BUF_SIZE];
static volatile int raw_head;
static volatile int raw_tail;

static void keyboard_irq(struct regs *r)
{
    (void)r;
    /* 状态寄存器 bit5=1 表示 0x60 里的数据来自鼠标: 交还给鼠标处理 */
    if (inb(KB_STATUS) & 0x20) {
        mouse_input_byte(inb(KB_DATA));
        return;
    }
    u8 sc = inb(KB_DATA);
    int next = (raw_head + 1) % KB_BUF_SIZE;
    if (next != raw_tail) {
        raw_buf[raw_head] = sc;
        raw_head = next;
    }
    softirq_raise(SOFTIRQ_KBD);
}

static void kbd_bottom_half(void)
{
    while (raw_tail != raw_head) {
        u8 sc = raw_buf[raw_tail];
        raw_tail = (raw_tail + 1) % KB_BUF_SIZE;
        keyboard_input_byte(sc);
    }
}

void keyboard_init(void)
{
    kb_head = kb_tail = 0;
    raw_head = raw_tail = 0;
    shift_down = false;
    ctrl_down = false;
    alt_down = false;
    caps_lock = false;
    ext_pending = false;
    softirq_set(SOFTIRQ_KBD, kbd_bottom_half);
    irq_install_handler(1, keyboard_irq);
}

bool keyboard_has_data(void) { return kb_head != kb_tail; }

int keyboard_read(void)
{
    if (!keyboard_has_data())
        return -1;
    int v = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return v;
}

int keyboard_getchar(void)
{
    while (!keyboard_has_data())
        cpu_relax();
    return keyboard_read();
}