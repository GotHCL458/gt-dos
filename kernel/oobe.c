/* ==========================================================================
 *  GT-DOS OOBE (Out-of-Box Experience) 首启向导
 *  ---------------------------------------------------------------------------
 *  首次开机 (GTOS.CFG 中 oobe_done=0) 时运行:
 *    1) 选择语言 (English / 中文)
 *    2) 设置主机名
 *    3) 创建管理员账户 (默认 admin + 口令)
 *    4) 选择控制台主题色
 *  完成/中断后写 oobe_done=1 并保存配置, 之后开机直接进入登录界面.
 * ========================================================================== */
#include "gt.h"

static void hr(void)
{
    console_puts("  ----------------------------------------\n");
}

static int ask_line(const char *prompt, char *buf, int max)
{
    for (;;) {
        int r = ui_read_line(prompt, buf, max, 0, false);
        if (r == -1)
            power_reboot();
        if (r == 1)
            return -1;                /* Ctrl-C: 使用默认 / 跳过本步 */
        if (buf[0])
            return 0;                 /* 空输入: 重新询问, 不跳步 */
    }
}

static int ask_choice(const char *prompt, int min, int max)
{
    char buf[16];
    for (;;) {
        if (ask_line(prompt, buf, sizeof(buf)) != 0)
            return -1;
        int ok = 1;
        for (char *p = buf; *p; p++)
            if (!isdigit_(*p)) ok = 0;
        if (!ok) { ui_err("  Please enter a number.\n"); continue; }
        int v = atoi_dec(buf);
        if (v < min || v > max) {
            ui_err("  Out of range (%d-%d).\n", min, max);
            continue;
        }
        return v;
    }
}

void oobe_run(void)
{
    char buf[64];

    console_clear();
    tui_status_refresh();
    tui_frame_open(L("oobe.title"));
    ui_info("%s", L("oobe.welcome"));

    /* ---- 步骤 1: 语言 ---- */
    ui_info("  [1/4] %s\n", L("oobe.lang"));
    kprintf("%s\n", L("oobe.lang_en"));
    kprintf("%s\n", L("oobe.lang_zh"));
    int lc = ask_choice(L("oobe.lang_q"), 1, 2);
    i18n_set_lang(lc == 2 ? 1 : 0);
    gcfg.lang = (char)(lc == 2 ? 1 : 0);
    if (lc == 2)
        ui_ok("%s", L("oobe.lang_set"));
    hr();

    /* ---- 步骤 2: 主机名 ---- */
    ui_info("  [2/4] %s\n", L("oobe.host"));
    kprintf("%s", L("oobe.host_desc"));
    if (ask_line(L("oobe.host_q"), buf, sizeof(buf)) == 0 && buf[0]) {
        size_t l = strlen(buf);
        if (l >= sizeof(gcfg.hostname))
            l = sizeof(gcfg.hostname) - 1;
        memcpy(gcfg.hostname, buf, l);
        gcfg.hostname[l] = '\0';
        ui_ok(L("oobe.host_set"), gcfg.hostname);
    } else {
        ui_info(L("oobe.host_def"), gcfg.hostname);
    }
    hr();

    /* ---- 步骤 3: 管理员账户 ---- */
    ui_info("  [3/4] %s\n", L("oobe.admin"));
    char admin_name[UNAME_MAX] = "admin";
    if (fat_ready()) {
        for (;;) {
            if (ask_line(L("oobe.admin_q"), buf, sizeof(buf)) != 0) {
                ui_info("%s", L("oobe.admin_def"));
                break;
            }
            if (!buf[0]) {
                ui_info("%s", L("oobe.admin_def"));
                break;
            }
            if (user_add(buf, "", UF_ADMIN) || user_exists(buf)) {
                ui_ok(L("oobe.admin_created"), buf);
                strncpy(admin_name, buf, sizeof(admin_name) - 1);
                admin_name[sizeof(admin_name) - 1] = '\0';
                strncpy(gcfg.last_user, buf, sizeof(gcfg.last_user) - 1);
                break;
            }
            ui_err(L("oobe.admin_bad"), UNAME_MAX - 1);
        }

        /* 可选口令 */
        char p1[64], p2[64];
        ui_info("%s", L("oobe.pass_q"));
        if (ui_read_line(L("oobe.pass"), p1, sizeof(p1), '*', false) == -1) {
            power_reboot();
        }
        if (p1[0]) {
            if (ui_read_line(L("oobe.pass2"), p2, sizeof(p2), '*',
                             false) == -1) {
                power_reboot();
            }
            if (!strcmp(p1, p2)) {
                if (user_setpass(admin_name, p1))
                    ui_ok(L("oobe.pass_set"), admin_name);
                else
                    ui_err("%s", L("oobe.pass_no"));
                memset(p1, 0, sizeof(p1));
                memset(p2, 0, sizeof(p2));
            } else {
                memset(p1, 0, sizeof(p1));
                memset(p2, 0, sizeof(p2));
                ui_err("%s", L("oobe.pass_mismatch"));
            }
        }
        user_save();
    } else {
        ui_err("%s", L("oobe.skip"));
    }
    hr();

    /* ---- 步骤 4: 主题 ---- */
    ui_info("  [4/4] %s\n", L("oobe.theme"));
    kprintf("%s\n", L("oobe.theme1"));
    kprintf("%s\n", L("oobe.theme2"));
    kprintf("%s\n", L("oobe.theme3"));
    kprintf("%s\n", L("oobe.theme4"));
    int c = ask_choice(L("oobe.theme_q"), 1, 4);
    switch (c) {
    case 2: gcfg.theme_fg = C_GREEN;  gcfg.theme_bg = C_BLACK; break;
    case 3: gcfg.theme_fg = C_YELLOW; gcfg.theme_bg = C_BLACK; break;
    case 4: gcfg.theme_fg = C_WHITE;  gcfg.theme_bg = C_BLUE;  break;
    default: gcfg.theme_fg = C_LGRAY; gcfg.theme_bg = C_BLACK; break;
    }
    cfg_apply_theme();
    ui_info("%s", L("oobe.theme_ok"));
    hr();

    /* ---- 完成 ---- */
    gcfg.oobe_done = true;
    cfg_save();
    ui_ok("\n%s", L("oobe.done"));
    tui_frame_close();
    timer_sleep_ms(800);
}
