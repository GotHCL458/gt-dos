/* 图形化 OOBE (首启向导) 与登录界面.
 *
 * 仅在 gfx 可用且桌面未激活时启用 (启动阶段 / 纯文本态). 绘制写入 gfx 后台
 * 缓冲并提交显存; 控制台绘制用 vga_hold_display(true) 暂停, 但串口输出与
 * 历史缓冲照常写入 —— 因此与既有自动化测试 (串口握手) 完全兼容.
 *
 * 输入统一走 ui_read_key() (PS/2 + 串口), 文本字段只画在图形界面上, 不再向
 * 串口回显键入内容; 返回码与 ui_read_line() 一致: 0=提交, 1=Ctrl-C, -1=重启. */
#include "gt.h"

/* 配色 (与 gui.c 桌面一致) */
#define C_PANEL   0x1b2434
#define C_TITLE   0x1f4e8a
#define C_TITLE2  0x2a6ab0
#define C_EDGE_L  0xbfd0e8
#define C_EDGE_D  0x0c1220
#define C_TX      0xe8f0ff
#define C_MUTED   0x8fb6d8
#define C_ACCENT  0x66ccff
#define C_FIELD   0x0e1622

static int PW, PH;                              /* 面板 */
static int PX, PY;
static int TITLE_H;
static int CT_X, CT_Y, CT_W, CT_H;              /* 内容区 */
static int ST_Y;                                /* 状态行 */
static int LBL_Y;                               /* 字段标签 */
static int FX, FY, FW, FH;                      /* 输入框 */
static int FOOT_Y;

/* 是否启用图形认证界面: 图形可用且桌面未激活 (启动阶段 / 纯文本态).
   桌面内 login/logout 仍走终端, 避免与 gui_pump 桌面重绘冲突. */
bool gs_available(void)
{
    return gfx_active() && !gui_active();
}

static u32 lerp(u32 a, u32 b, int t)
{
    u32 ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    u32 br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    u32 r = ar + ((br - ar) * (u32)t) / 255u;
    u32 g = ag + ((bg - ag) * (u32)t) / 255u;
    u32 bl = ab + ((bb - ab) * (u32)t) / 255u;
    return (r << 16) | (g << 8) | bl;
}

/* 去掉前导空白与行尾换行, 只保留首行 (GUI 单行绘制) */
static void trim_line(const char *s, char *out, int cap)
{
    while (*s == '\n' || *s == '\r' || *s == ' ')
        s++;
    int n = 0;
    while (*s && *s != '\n' && *s != '\r' && n < cap - 1)
        out[n++] = *s++;
    out[n] = '\0';
}

/* ------------------------------------------------------------- 背景 */
static void gs_backdrop(void)
{
    if (png_draw_file_fit("WALLPAPER.PNG"))
        return;
    int W = gfx_w(), H = gfx_h();
    for (int y = 0; y < H; y += 4) {
        u32 c = lerp(0x0e1a30, 0x2f5a7a, (y * 255) / (H > 1 ? H - 1 : 1));
        gfx_fill(0, y, W, 4, c);
    }
    int tw = gfx_text_w("GT-DOS");
    gfx_text((W - tw) / 2, H / 5, "GT-DOS", 0x2a4a6a);
}

/* ------------------------------------------------------------- 面板 */
static void gs_card(const char *title, const char *sub)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    int gw = gfx_w(), gh = gfx_h();

    TITLE_H = ch + 8;
    PW = 60 * cw;
    if (PW > gw - 60) PW = gw - 60;
    if (PW < 40 * cw) PW = 40 * cw;
    PH = 20 * ch;
    if (PH > gh - 60) PH = gh - 60;
    if (PH < 12 * ch) PH = 12 * ch;
    PX = (gw - PW) / 2;
    PY = (gh - PH) / 2;

    CT_X = PX + 20;
    CT_W = PW - 40;
    ST_Y = PY + TITLE_H + 4;
    CT_Y = ST_Y + ch + 10;

    FOOT_Y = PY + PH - ch - 8;
    FH = ch + 10;
    FY = FOOT_Y - 8 - FH;
    LBL_Y = FY - ch - 6;
    CT_H = LBL_Y - 8 - CT_Y;
    if (CT_H < ch)
        CT_H = ch;

    FX = CT_X;
    FW = CT_W;

    gs_backdrop();

    /* 阴影 + 面板 + 标题栏 */
    gfx_fill(PX + 10, PY + 10, PW, PH, 0x05080e);
    gfx_fill(PX, PY, PW, PH, C_PANEL);
    gfx_bevel(PX, PY, PW, PH, C_EDGE_L, C_EDGE_D);
    gfx_fill(PX + 2, PY + 2, PW - 4, TITLE_H - 2, C_TITLE);
    gfx_fill(PX + 2, PY + 2, PW - 4, (TITLE_H - 2) / 2, C_TITLE2);
    gfx_fill(PX + 2, PY + 2, PW - 4, 1, 0xdfefff);
    gfx_text(PX + 14, PY + (TITLE_H - ch) / 2, title, C_TX);

    /* 副标题 / 提示 (状态行) */
    gfx_fill(CT_X, ST_Y, CT_W, ch, C_PANEL);
    if (sub) {
        char tmp[160];
        trim_line(sub, tmp, sizeof(tmp));
        if (tmp[0])
            gfx_text(CT_X, ST_Y, tmp, C_MUTED);
    }

    /* 页脚提示 */
    gfx_text(CT_X, FOOT_Y, "Enter = OK / defaults    Ctrl-C = skip", C_MUTED);

    gfx_flip();
    gfx_poked = false;
}

/* 内容区: 标题 + 若干行 (自动裁剪, 只画首行) */
static void gs_content(const char *heading, const char *const *lines, int n)
{
    int ch = gfx_cell_h();
    char tmp[160];

    gfx_fill(CT_X, CT_Y, CT_W, CT_H, C_PANEL);
    if (heading) {
        gfx_text(CT_X, CT_Y, heading, C_ACCENT);
    }
    int y = CT_Y + ch + 8;
    for (int i = 0; i < n && y + ch <= CT_Y + CT_H; i++, y += ch + 4) {
        if (!lines[i])
            continue;
        trim_line(lines[i], tmp, sizeof(tmp));
        if (tmp[0])
            gfx_text(CT_X + 8, y, tmp, C_TX);
    }
    gfx_flip_rect(CT_X - 2, CT_Y - 2, CT_W + 4, CT_H + 4);
}

/* 状态行: 提示 / 错误 */
static void gs_status(const char *s)
{
    int ch = gfx_cell_h();
    char tmp[160];
    gfx_fill(CT_X, ST_Y, CT_W, ch, C_PANEL);
    if (s) {
        trim_line(s, tmp, sizeof(tmp));
        if (tmp[0])
            gfx_text(CT_X, ST_Y, tmp, 0xffc060);
    }
    gfx_flip_rect(CT_X - 2, ST_Y - 2, CT_W + 4, ch + 4);
}

/* --------------------------------------------------------- 文本输入框 */
static void gs_field_draw(const char *label, const char *buf, int len,
                          int mask, int cur)
{
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    char tmp[160];
    int top = LBL_Y;
    int hgt = (FY + FH) - LBL_Y;

    gfx_fill(CT_X, top, CT_W, hgt, C_PANEL);

    if (label) {
        trim_line(label, tmp, sizeof(tmp));
        if (tmp[0])
            gfx_text(CT_X, LBL_Y, tmp, C_MUTED);
    }

    gfx_fill(FX, FY, FW, FH, C_FIELD);
    gfx_bevel(FX, FY, FW, FH, C_EDGE_L, C_EDGE_D);

    int tx = FX + 8, ty = FY + (FH - ch) / 2;
    int maxc = (FW - 16) / cw;                   /* 可见字符数 */
    if (maxc < 1) maxc = 1;
    int start = 0;
    if (cur >= maxc)
        start = cur - maxc + 1;
    for (int i = start; i < len && i - start < maxc; i++) {
        char c = mask ? (char)mask : buf[i];
        gfx_char(tx + (i - start) * cw, ty, (u8)c, C_TX);
    }

    /* 光标 */
    int cx = tx + (cur - start) * cw;
    if (cx > FX + FW - 3)
        cx = FX + FW - 3;
    gfx_fill(cx, FY + 3, 2, FH - 6, C_ACCENT);

    gfx_flip_rect(CT_X - 2, LBL_Y - 2, CT_W + 4, hgt + 4);
}

/* 读取一个字段. 返回 0=回车, 1=Ctrl-C, -1=Ctrl-Alt-Del */
static int gs_read_field(const char *prompt, char *buf, int max, int mask)
{
    int len = 0, cur = 0;
    buf[0] = '\0';

    console_puts(prompt);                        /* 串口 + 历史 (屏幕暂停) */
    gs_field_draw(prompt, buf, len, mask, cur);

    for (;;) {
        int k = ui_read_key();

        if (k == KEY_REBOOT)
            return -1;
        if (k == '\r' || k == '\n') {
            buf[len] = '\0';
            return 0;
        }
        if (k == 3) {                            /* Ctrl-C */
            buf[0] = '\0';
            return 1;
        }
        if (k == KEY_LEFT) {
            if (cur > 0) { cur--; gs_field_draw(prompt, buf, len, mask, cur); }
            continue;
        }
        if (k == KEY_RIGHT) {
            if (cur < len) { cur++; gs_field_draw(prompt, buf, len, mask, cur); }
            continue;
        }
        if (k == KEY_HOME || k == 1) {
            cur = 0;
            gs_field_draw(prompt, buf, len, mask, cur);
            continue;
        }
        if (k == KEY_END || k == 5) {
            cur = len;
            gs_field_draw(prompt, buf, len, mask, cur);
            continue;
        }
        if (k == KEY_DEL) {
            if (cur < len) {
                for (int i = cur; i < len - 1; i++)
                    buf[i] = buf[i + 1];
                len--;
                buf[len] = '\0';
                gs_field_draw(prompt, buf, len, mask, cur);
            }
            continue;
        }
        if (k == '\b' || k == 127) {
            if (cur > 0) {
                for (int i = cur; i < len; i++)
                    buf[i - 1] = buf[i];
                len--;
                cur--;
                buf[len] = '\0';
                gs_field_draw(prompt, buf, len, mask, cur);
            }
            continue;
        }
        if (k == 27) {                           /* ESC: 清空 */
            len = cur = 0;
            buf[0] = '\0';
            gs_field_draw(prompt, buf, len, mask, cur);
            continue;
        }
        if (k >= 32 && k <= 126 && len + 1 < max) {
            for (int i = len; i > cur; i--)
                buf[i] = buf[i - 1];
            buf[cur] = (char)k;
            len++;
            cur++;
            buf[len] = '\0';
            gs_field_draw(prompt, buf, len, mask, cur);
        }
    }
}

/* 行输入 (空回车 = 采用默认值, Ctrl-C 返回 -1) */
static int gs_ask_line(const char *prompt, char *buf, int max)
{
    int r = gs_read_field(prompt, buf, max, 0);
    if (r == -1)
        power_reboot();
    if (r == 1)
        return -1;
    return 0;
}

/* 数字选择 (与旧 OOBE ask_choice 语义一致) */
static int gs_ask_choice(const char *prompt, int min, int max)
{
    char buf[16];
    for (;;) {
        if (gs_ask_line(prompt, buf, sizeof(buf)) != 0)
            return -1;
        if (buf[0] == '\0')
            return min;               /* 空回车 = 默认选项 */
        int ok = 1;
        for (char *p = buf; *p; p++)
            if (!isdigit_(*p)) ok = 0;
        if (!ok) {
            ui_err("  Please enter a number.\n");
            gs_status("Please enter a number.");
            continue;
        }
        int v = atoi_dec(buf);
        if (v < min || v > max) {
            ui_err("  Out of range (%d-%d).\n", min, max);
            gs_status("Out of range");
            continue;
        }
        return v;
    }
}

/* ------------------------------------------------------------- OOBE */
void gs_oobe_run(void)
{
    char buf[64];

    vga_hold_display(true);
    console_clear();
    tui_status_refresh();
    tui_frame_open(L("oobe.title"));

    gs_card(L("oobe.title"), L("oobe.welcome"));
    gs_content(L("oobe.title"), (const char *[]){ L("oobe.welcome") }, 1);

    ui_info("%s", L("oobe.welcome"));
    ui_info("%s", L("oobe.defaults"));

    /* ---- 步骤 1: 语言 ---- */
    ui_info("  [1/3] %s\n", L("oobe.lang"));
    kprintf("%s\n", L("oobe.lang_en"));
    kprintf("%s\n", L("oobe.lang_zh"));
    gs_content(L("oobe.lang"),
               (const char *[]){ L("oobe.lang_en"), L("oobe.lang_zh") }, 2);
    int lc = gs_ask_choice(L("oobe.lang_q"), 1, 2);
    i18n_set_lang(lc == 2 ? 1 : 0);
    gcfg.lang = (char)(lc == 2 ? 1 : 0);
    if (lc == 2) {
        ui_ok("%s", L("oobe.lang_set"));
        gs_status(L("oobe.lang_set"));
    }

    /* ---- 步骤 2: 主机名 ---- */
    ui_info("  [2/3] %s\n", L("oobe.host"));
    kprintf("%s", L("oobe.host_desc"));
    gs_content(L("oobe.host"), (const char *[]){ L("oobe.host_desc") }, 1);
    if (gs_ask_line(L("oobe.host_q"), buf, sizeof(buf)) == 0 && buf[0]) {
        size_t l = strlen(buf);
        if (l >= sizeof(gcfg.hostname))
            l = sizeof(gcfg.hostname) - 1;
        memcpy(gcfg.hostname, buf, l);
        gcfg.hostname[l] = '\0';
        ui_ok(L("oobe.host_set"), gcfg.hostname);
        gs_status(L("oobe.host_set"));
    } else {
        ui_info(L("oobe.host_def"), gcfg.hostname);
        gs_status(L("oobe.host_def"));
    }

    /* ---- 步骤 3: 管理员账户 ---- */
    ui_info("  [3/3] %s\n", L("oobe.admin"));
    gs_content(L("oobe.admin"), (const char *[]){ L("oobe.admin_q") }, 1);

    char admin_name[UNAME_MAX] = "admin";
    if (fat_ready()) {
        for (;;) {
            if (gs_ask_line(L("oobe.admin_q"), buf, sizeof(buf)) != 0) {
                ui_info("%s", L("oobe.admin_def"));
                gs_status(L("oobe.admin_def"));
                break;
            }
            if (!buf[0]) {
                ui_info("%s", L("oobe.admin_def"));
                gs_status(L("oobe.admin_def"));
                break;
            }
            if (user_add(buf, "", UF_ADMIN) || user_exists(buf)) {
                ui_ok(L("oobe.admin_created"), buf);
                gs_status(L("oobe.admin_created"));
                strncpy(admin_name, buf, sizeof(admin_name) - 1);
                admin_name[sizeof(admin_name) - 1] = '\0';
                strncpy(gcfg.last_user, buf, sizeof(gcfg.last_user) - 1);
                break;
            }
            ui_err(L("oobe.admin_bad"), UNAME_MAX - 1);
            gs_status(L("oobe.admin_bad"));
        }

        /* 可选口令 */
        char p1[64], p2[64];
        ui_info("%s", L("oobe.pass_q"));
        gs_content(L("oobe.pass_q"), (const char *[]){ L("oobe.pass_q") }, 1);
        if (gs_read_field(L("oobe.pass"), p1, sizeof(p1), '*') == -1)
            power_reboot();
        if (p1[0]) {
            if (gs_read_field(L("oobe.pass2"), p2, sizeof(p2), '*') == -1)
                power_reboot();
            if (!strcmp(p1, p2)) {
                if (user_setpass(admin_name, p1)) {
                    ui_ok(L("oobe.pass_set"), admin_name);
                    gs_status(L("oobe.pass_set"));
                } else {
                    ui_err("%s", L("oobe.pass_no"));
                    gs_status(L("oobe.pass_no"));
                }
                memset(p1, 0, sizeof(p1));
                memset(p2, 0, sizeof(p2));
            } else {
                memset(p1, 0, sizeof(p1));
                memset(p2, 0, sizeof(p2));
                ui_err("%s", L("oobe.pass_mismatch"));
                gs_status(L("oobe.pass_mismatch"));
            }
        }
        user_save();
    } else {
        ui_err("%s", L("oobe.skip"));
        gs_status(L("oobe.skip"));
    }

    /* ---- 完成 ---- */
    gcfg.oobe_done = true;
    cfg_save();
    ui_ok("\n%s", L("oobe.done"));
    gs_content(L("oobe.done"), (const char *[]){ L("oobe.done") }, 1);
    tui_frame_close();
    timer_sleep_ms(800);

    vga_hold_display(false);
    vga_refresh();
}

/* ------------------------------------------------------------- 登录 */
static const struct gt_user *user_by_name(const char *name)
{
    for (int i = 0; i < user_count(); i++) {
        const struct gt_user *u = user_get(i);
        if (u && !strncasecmp(u->name, name, UNAME_MAX))
            return u;
    }
    return 0;
}

const struct gt_user *gs_login_screen(void)
{
    char name[UNAME_MAX];
    char pass[64];
    const struct gt_user *result = 0;

    vga_hold_display(true);
    console_puts("\n");
    tui_frame_open(L("login.title"));
    gs_card(L("login.title"), L("login.hint"));
    gs_content(L("login.title"), (const char *[]){ L("login.hint") }, 1);

    for (int tries = 0; tries < 5; tries++) {
        int r = gs_read_field(L("login.user"), name, sizeof(name), 0);
        if (r == -1)
            power_reboot();
        if (r == 1)
            break;                              /* Ctrl-C: 跳过登录 */
        if (name[0] == '\0') {
            ui_err("%s", L("login.empty"));
            gs_status(L("login.empty"));
            continue;
        }
        if (!user_exists(name)) {
            ui_err("%s", L("login.nouser"));
            gs_status(L("login.nouser"));
            continue;
        }

        r = gs_read_field(L("login.pass"), pass, sizeof(pass), '*');
        if (r == -1)
            power_reboot();
        if (r == 1)
            break;

        if (user_check(name, pass)) {
            const struct gt_user *u = user_by_name(name);
            if (u)
                result = u;
            ui_ok(L("login.welcome"), name,
                  (u && (u->flags & UF_ADMIN)) ? " (admin)" : "");
            gs_status(L("login.welcome"));
            break;
        }
        ui_err("%s", L("login.wrong"));
        gs_status(L("login.wrong"));
        timer_sleep_ms(600);
    }

    vga_hold_display(false);
    vga_refresh();

    if (!result) {                              /* 兜底: 默认 admin */
        result = user_by_name("admin");
        if (!result)
            result = user_current();
    }
    return result;
}
