/* ==========================================================================
 *  GT-DOS 内核主入口
 *  到这里时: CPU 已处于 32 位保护模式, 内核已被引导扇区加载到 0x10000
 *  启动流程: 控制台 -> 中断 -> 外设 -> 磁盘 -> 配置 -> OOBE -> 登录 -> Shell
 * ========================================================================== */
#include "gt.h"

#define BOOT_DRIVE_ADDR 0x0600

void kmain(void)
{
    /* 1) 内核容器自检: 必须在任何代码改写 .data 之前完成 */
    int kcheck = kernel_self_check();

    /* 2) 尽早初始化串口, 保证后续任何输出都能送到终端 */
    console_init();

    if (kcheck != 0) {
        kprintf("[boot] KERNEL CONTAINER INTEGRITY CHECK FAILED (%d)\n",
                kcheck);
        panic("kernel image corrupted");
    }
    kprintf("[boot] kernel container GTK1 verified (SHA-256)\n");

    /* 3) 建立中断体系 */
    idt_init();
    pic_init();
    softirq_init();
    task_init();                  /* 登记内核系统服务任务 (SYS) */
    sched_init();                 /* 主线程 + 调度器 */

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
        if (gfx_active())
            kprintf("[boot] video: %dx%d x32, font x%d, text %dx%d\n",
                    gfx_w(), gfx_h(), gfx_scale(),
                    gfx_w() / gfx_cell_w(), gfx_h() / gfx_cell_h());
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

    /* 5b) 网络 (RTL8139, 默认 10.0.2.15/24; DHCP 可用 NET/DHCP 命令) */
    if (net_init()) {
        char ipbuf[20];
        net_ip_str(net_ip(), ipbuf);
        kprintf("[boot] net: RTL8139 %s/24\n", ipbuf);
    } else {
        kprintf("[boot] no network card\n");
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

    /* 10.5) 默认图形桌面 (登录后自动进入, 可用 CONFIG GUI OFF 关闭) */
    if (gcfg.gui_default && gfx_active())
        gui_enter();

    /* 11) 交给 Shell */
    shell_run();

    panic("shell returned unexpectedly");
}