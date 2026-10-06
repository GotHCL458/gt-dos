/* ==========================================================================
 *  GT-DOS 内核主入口
 *  到这里时: CPU 已处于 32 位保护模式, 内核已被引导扇区加载到 0x10000
 *  启动流程: 控制台 -> 中断 -> 外设 -> 磁盘 -> 配置 -> OOBE -> 登录 -> Shell
 * ========================================================================== */
#include "gt.h"

#define BOOT_DRIVE_ADDR 0x0600

void kmain(void)
{
    /* 1) 尽早初始化串口, 保证后续任何输出都能送到终端 */
    console_init();

    /* 2) 建立中断体系 */
    idt_init();
    pic_init();
    softirq_init();

    /* 3) 外设 */
    timer_init(100);
    keyboard_init();
    mouse_init(shell_wheel_cb);

    /* 开中断 (splash 的进度条依赖 PIT tick) */
    cpu_sti();

    /* 4) TUI 启动画面 */
    tui_splash();

    kprintf("[boot] COM1 / IDT / PIC / PIT / keyboard / mouse(%s) ready\n",
            mouse_present() ? "wheel" : "none");

    /* 5) 磁盘: ATA + FAT */
    if (ata_init()) {
        vga_load_font();              /* 保护模式用 ATA 读取汉字字库 */
        vga_enter_gfx();              /* 字库就绪后切换图形文本模式 (去 64 槽限制) */
        kprintf("[boot] ATA disk: %u sectors (%u MB)\n",
                ata_sectors(), (ata_sectors() * 512u) / (1024u * 1024u));
        if (fat_mount()) {
            kprintf("[boot] FAT mounted %d drive(s), current='%c'",
                    fat_drive_count(), fat_drive());
            for (int i = 0; i < fat_drive_count(); i++)
                kprintf(" [%c]", fat_drive_letter(i));
            kprintf("\n");
        } else
            kprintf("[boot] no FAT volume found\n");
    } else {
        kprintf("[boot] no ATA disk (attach one with qemu -hda)\n");
    }

    /* 6) 配置 + 用户库 */
    if (cfg_load())
        kprintf("[boot] config loaded (host='%s', oobe=%s)\n",
                gcfg.hostname, gcfg.oobe_done ? "done" : "pending");
    else
        kprintf("[boot] config: defaults (no disk)\n");
    i18n_set_lang(gcfg.lang);
    cfg_apply_theme();
    user_init();
    kprintf("[boot] user database: %d user(s)\n", user_count());

    /* 7) 首启向导 (OOBE) */
    if (!gcfg.oobe_done) {
        kprintf("[boot] first boot detected -> starting OOBE\n");
        oobe_run();
    }

    /* 9) 登录 */
    user_login(ui_login_screen());
    cfg_apply_theme();
    kprintf("[boot] signed in as '%s'\n", user_name());

    /* 10) 状态栏上线 */
    tui_status_refresh();

    /* 11) 交给 Shell */
    shell_run();

    panic("shell returned unexpectedly");
}