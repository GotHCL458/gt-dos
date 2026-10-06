/* ==========================================================================
 *  GT-DOS 国际化 (i18n)
 *  ---------------------------------------------------------------------------
 *  双语字表: 英文 / 简体中文. 中文串以 UTF-8 字面量存储,
 *  由 vga.c 的 UTF-8 解码 + 动态字库渲染.
 *  L(key) 按当前语言取文案; 缺省返回英文 (即 key 本身对应的 en 字段).
 * ========================================================================== */
#include "gt.h"

struct entry {
    const char *key;
    const char *en;
    const char *zh;
};

static const struct entry table[] = {
    /* ---- OOBE ---- */
    { "oobe.title",      "GT-DOS Setup",              "GT-DOS 设置" },
    { "oobe.welcome",
      "\n     Welcome to GT-DOS!\n"
      "     Let's set up your machine. This takes one minute.\n"
      "     (Ctrl-C on any step = keep default / skip)\n\n",
      "\n     欢迎使用 GT-DOS！\n"
      "     让我们完成初始设置，只需一分钟。\n"
      "     (任意步骤按 Ctrl-C = 使用默认值 / 跳过)\n\n" },
    { "oobe.lang",       "Language",                   "语言" },
    { "oobe.lang_en",    "  1) English",              "  1) English" },
    { "oobe.lang_zh",    "  2) 中文 (Chinese)",       "  2) 中文 (Chinese)" },
    { "oobe.lang_q",     "  Choose [1]: ",            "  请选择 [1]: " },
    { "oobe.lang_set",   "  Language set to Chinese.\n", "  已选择中文。\n" },
    { "oobe.host",       "Machine name",              "机器名" },
    { "oobe.host_desc",  "  This name appears in the prompt: <name>:\\>\n",
      "  该名称将显示在提示符中: <name>:\\>\n" },
    { "oobe.host_q",     "  Host name [GT-DOS]: ",    "  主机名 [GT-DOS]: " },
    { "oobe.host_set",   "  Host name set to '%s'.\n", "  主机名已设为 '%s'。\n" },
    { "oobe.host_def",   "  Keeping default '%s'.\n", "  保留默认值 '%s'。\n" },
    { "oobe.admin",      "Administrator account",     "管理员账户" },
    { "oobe.admin_q",    "  New user name [admin]: ", "  新用户名称 [admin]: " },
    { "oobe.admin_def",  "  Using default 'admin' (no password).\n",
      "  使用默认账户 'admin' (无口令)。\n" },
    { "oobe.admin_created", "  Admin user '%s' created (no password yet).\n",
      "  管理员账户 '%s' 已创建 (尚未设置口令)。\n" },
    { "oobe.admin_bad",
      "  Invalid or existing name. Letters/digits/_/- only, max %d chars, not starting with a digit.\n",
      "  名称无效或已存在。仅限字母/数字/_/-，最多 %d 字符，不能以数字开头。\n" },
    { "oobe.pass_q",     "  Set a login password? (optional)\n",
      "  是否设置登录口令? (可选)\n" },
    { "oobe.pass",       "  Password (empty = none): ", "  口令 (留空=无): " },
    { "oobe.pass2",      "  Confirm password:      ", "  确认口令:        " },
    { "oobe.pass_set",   "  Password set for '%s'.\n", "  已为 '%s' 设置口令。\n" },
    { "oobe.pass_no",    "  User not found - password not set.\n",
      "  用户不存在 - 未设置口令。\n" },
    { "oobe.pass_mismatch", "  Passwords do not match - no password set.\n",
      "  两次口令不一致 - 未设置口令。\n" },
    { "oobe.theme",      "Console theme",             "控制台主题" },
    { "oobe.theme1",     "    1) Gray on black (classic)", "    1) 灰底黑字 (经典)" },
    { "oobe.theme2",     "    2) Green on black (terminal)", "    2) 绿底黑字 (终端)" },
    { "oobe.theme3",     "    3) Yellow on black (amber-ish)", "    3) 黄底黑字 (琥珀)" },
    { "oobe.theme4",     "    4) White on blue",      "    4) 白底蓝字" },
    { "oobe.theme_q",    "  Choose [1]: ",            "  请选择 [1]: " },
    { "oobe.theme_ok",   "  Theme applied.\n",        "  主题已应用。\n" },
    { "oobe.done",       "  Setup complete. Enjoy GT-DOS!\n",
      "  设置完成。祝使用愉快！\n" },
    { "oobe.skip",       "  No disk available - skipping account setup.\n",
      "  无可用磁盘 - 跳过账户设置。\n" },

    /* ---- 登录 ---- */
    { "login.title",     "GT-DOS Login",             "GT-DOS 登录" },
    { "login.hint",      "  (Ctrl-C = skip, admin required)\n\n",
      "  (Ctrl-C = 跳过, 需管理员账户)\n\n" },
    { "login.empty",     "  User name cannot be empty.\n",
      "  用户名不能为空。\n" },
    { "login.user",      "  User: ",                  "  用户: " },
    { "login.pass",      "  Password: ",              "  口令: " },
    { "login.nouser",    "  No such user.\n",         "  用户不存在。\n" },
    { "login.wrong",     "  Wrong password.\n",       "  口令错误。\n" },
    { "login.welcome",   "  Welcome, %s%s.\n",        "  欢迎, %s%s。\n" },

    /* ---- 启动/状态 ---- */
    { "splash.sub",      "        a tiny DOS-like OS  --  NASM + C (clang/LLVM)\n",
      "        一个迷你 DOS 风格操作系统 -- NASM + C (clang/LLVM)\n" },
    { "splash.s1",       "Loading kernel",            "加载内核" },
    { "splash.s2",       "Mounting disks",            "挂载磁盘" },
    { "splash.s3",       "Starting services",         "启动服务" },
    { "status.boot",     "GT-DOS is booting",         "GT-DOS 正在启动" },
    { "status.ready",    "Ready",                     "就绪" },

    /* ---- Shell 提示 ---- */
    { "shell.banner",    "GT-DOS v0.6  --  Chinese font, MBR boot, protected system\n",
      "GT-DOS v0.6  --  中文字库, MBR 引导, 系统保护\n" },
    { "shell.hint",
      "Signed in as '%s' on '%s'. Type HELP for commands.\n",
      "已登录 '%s'@'%s'。输入 HELP 查看命令。\n" },
    { "shell.sysprot",   "System file/folder protected.\n",
      "系统文件/目录受保护。\n" },
    { "shell.badcmd",    "Bad command: %s  (type HELP)\n",
      "无效命令: %s  (输入 HELP)\n" },
    { "shell.perm",      "Permission denied: admin required.\n",
      "权限不足: 需要管理员。\n" },

    /* ---- HELP (图形文本模式无字模槽限制, 单页完整输出) ---- */
    { "help.text",
      "GT-DOS Commands:\n"
      "  HELP / VER / CLS / ECHO <text>\n"
      "  DATE / TIME / MEM / UPTIME / CPUID / BEEP\n"
      "  SERIAL ON|OFF     toggle serial output\n"
      "  COLOR <0-15>      set text color\n"
      "  DIV0              raise divide-by-zero (test)\n"
      "  GUI               enter graphical desktop\n"
      "  RES <W>x<H>|list  change resolution (640x480..1280x1024)\n"
      "  WHOAMI / USERS / LOGIN / LOGOUT\n"
      "  PASSWD [user]     (self only unless admin)\n"
      "  USERADD <n> [-a] / USERDEL <n>      [admin]\n"
      "  HOSTNAME [name]                     [set=admin]\n"
      "  CONFIG SHOW|RESET / OOBE            [admin]\n"
      "  APIC ON|OFF / IRQSTAT  interrupt stats\n"
      "  REBOOT / SHUTDOWN                   [admin]\n"
      "  (Ctrl+Alt+Del = emergency reboot)\n"
      "Disk (FAT):\n"
      "  DISK / MOUNT / DF / DRIVES\n"
      "  DIR|LS [/A] [path] / CD [dir] / PWD / TREE\n"
      "  TYPE|CAT <f> / PUT <f> <text> / HEAD [-n] <f> / WC <f>\n"
      "  DEL|RM <f> / MKDIR <d> / RMDIR <d> / TOUCH <f>\n"
      "  CP|COPY <src> <dst> / MV|REN <src> <dst>\n"
      "  ATTRIB <f> [+r|-r][+h|-h][+s|-s][+a|-a]\n"
      "  DIR /A shows hidden/system. \\GT-DOS\\ is protected.\n"
      "\n"
      "Edit: arrows/Home/End/Del, Up/Down=history.\n"
      "Scroll: wheel / Ctrl+Up/Down / PgUp/PgDn.\n",
      "GT-DOS 命令一览:\n"
      "  HELP VER CLS ECHO <文本>\n"
      "  DATE TIME MEM UPTIME CPUID BEEP\n"
      "  SERIAL ON|OFF  串口开关\n"
      "  COLOR 0-15     文字颜色\n"
      "  DIV0           除零测试\n"
      "  GUI            进入图形桌面\n"
      "  RES <宽>x<高>|list  切换分辨率 (640x480..1280x1024)\n"
      "  WHOAMI USERS LOGIN LOGOUT\n"
      "  PASSWD [用户]  修改口令 (非管理员仅限自己)\n"
      "  USERADD <名> [-a] / USERDEL <名>   [管理员]\n"
      "  HOSTNAME [名]  查看/设置主机名      [设置=管理员]\n"
      "  CONFIG SHOW|RESET / OOBE            [管理员]\n"
      "  APIC ON|OFF / IRQSTAT  中断统计\n"
      "  REBOOT / SHUTDOWN  重启/关机        [管理员]\n"
      "  (Ctrl+Alt+Del 紧急重启)\n"
      "磁盘 (FAT):\n"
      "  DISK MOUNT DF DRIVES\n"
      "  DIR|LS [/A] [路径] / CD [目录] / PWD / TREE\n"
      "  TYPE|CAT <文件> / PUT <文件> <文本> / HEAD [-n] <文件> / WC <文件>\n"
      "  DEL|RM <文件> / MKDIR <目录> / RMDIR <目录> / TOUCH <文件>\n"
      "  CP|COPY <源> <目标> / MV|REN <源> <目标>\n"
      "  ATTRIB <文件> [+r|-r][+h|-h][+s|-s][+a|-a]\n"
      "  DIR /A 显示隐藏/系统文件。\\GT-DOS\\ 受保护。\n"
      "\n"
      "编辑: 方向键/Home/End/Del, 上/下=历史。\n"
      "滚动: 鼠标滚轮 / Ctrl+上/下 / PgUp/PgDn。\n" },

    /* ---- GUI 桌面 ---- */
    { "gui.title",   "GT-DOS Desktop",              "GT-DOS 桌面" },
    { "gui.welcome", "Welcome to the GT-DOS desktop", "欢迎使用 GT-DOS 桌面" },
    { "gui.demo",    "Controls Demo",               "控件演示" },
    { "gui.about",   "About",                       "关于" },
    { "gui.sys",     "System Info",                 "系统信息" },
    { "gui.clicks",  "Clicks: ",                    "点击次数: " },
    { "gui.exit",    "Exit",                        "退出桌面" },
    { "gui.start",   "Start",                       "开始" },
    { "gui.press",   "Press me",                    "按我计数" },
    { "gui.chk",     "Enable effect",               "启用效果" },
    { "gui.close",   "Close",                       "关闭" },
    { "gui.about_t", "GT-DOS Graphical Desktop",    "GT-DOS 图形桌面" },
    { "gui.about_v", "Version 0.6  (NASM + C)",     "版本 0.6  (NASM + C)" },
    { "gui.about_d", "640x480 truecolor, double buffered", "640x480 真彩色, 双缓冲渲染" },
    { "gui.about_f", "Chinese glyphs from font.bin", "中文字模来自 font.bin 字库" },
    { "gui.sys_t",   "System Information",          "系统信息" },
    { "gui.sys_mem", "Conventional memory: ",       "常规内存: " },
    { "gui.sys_up",  "Uptime: ",                    "运行时间: " },
    { "gui.sys_drv", "FAT drives mounted: ",        "已挂载磁盘数: " },
    { 0, 0, 0 }
};

static int cur_lang = 0;

void i18n_set_lang(int lang) { cur_lang = lang ? 1 : 0; }
int  i18n_lang(void) { return cur_lang; }

const char *L(const char *key)
{
    for (int i = 0; table[i].key; i++) {
        if (!strcmp(table[i].key, key))
            return cur_lang ? table[i].zh : table[i].en;
    }
    return key;
}
