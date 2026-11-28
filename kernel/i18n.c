/* 国际化: 英文 / 简体中文双语字表. 中文串以 UTF-8 字面量存储, 由 vga.c 的
   UTF-8 解码 + 动态字库渲染. L(key) 按当前语言取文案, 缺省返回英文. */
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
    { "oobe.defaults",   "  Press Enter to accept the default value on any step.\n",
      "  任意步骤直接回车 = 采用默认值。\n" },
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
    { "splash.sub",      "        a tiny DOS-like OS\n",
      "        迷你 DOS 风格操作系统\n" },
    { "splash.s1",       "Loading kernel",            "加载内核" },
    { "splash.s2",       "Mounting disks",            "挂载磁盘" },
    { "splash.s3",       "Starting services",         "启动服务" },
    { "status.boot",     "GT-DOS is booting",         "GT-DOS 正在启动" },
    { "status.ready",    "Ready",                     "就绪" },

    /* ---- Shell 提示 ---- */
    { "shell.banner",    "GT-DOS v0.10\n",
      "GT-DOS v0.10\n" },
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
      "  DATE / TIME / MEM / KERNEL [VERIFY] / UPTIME / CPUID / BEEP\n"
      "  SERIAL ON|OFF     toggle serial output\n"
      "  COLOR <0-15>      set text color\n"
      "  DIV0              raise divide-by-zero (test)\n"
      "  WHOAMI / USERS / LOGIN / LOGOUT|EXIT\n"
      "  PASSWD [user]     (self only unless admin)\n"
      "  USERADD <n> [-a] / USERDEL <n>      [admin]\n"
      "  HOSTNAME [name]                     [set=admin]\n"
      "  CONFIG SHOW|RESET / OOBE            [admin]\n"
      "  APIC ON|OFF / IRQSTAT  interrupt stats\n"
      "  RUN <file.gtp>        load and run a program [admin]\n"
      "  TC <src.c> [-o out]   compile C subset -> .gtp\n"
      "  ASM <src.s> [-o out]  assemble x86-64 source -> .gtp\n"
      "  SDK: \\SDK\\TC.H  HELLO.C  FIB.C  NET.H\n"
      "  TASKS|PS / THREADS   tasks and thread table\n"
      "  DESKTOP|GUI / TEXTMODE  window desktop with mouse\n"
      "  SHOW <png|ico|svg> / PNGINFO <png>  view image / print header\n"
      "  SERVICE LIST|RUN <name>  SYS tasks protected\n"
      "  NET / DHCP / PING <ip> / TCP <ip> <port> [text]\n"
      "  UDP <ip> <port> [size] / ICMP errors via udp_recvfrom\n"
      "  DNS <name> / TFTP GET <ip[:port]> <f> [local]\n"
      "  HTTP GET <host[:port]> <path> [local]\n"
      "  REBOOT / SHUTDOWN                   [admin]\n"
      "  (Ctrl+Alt+Del = emergency reboot)\n"
      "Disk (exFAT):\n"
      "  DISK / MOUNT / DF / DRIVES\n"
      "  DIR|LS [/A] [path] / CD [dir] / PWD / TREE\n"
      "  TYPE <f> / CAT [-n] <f...> / RICH <f.md> / VIM <f>\n"
      "  PUT <f> <text> / APPEND <f> <line>\n"
      "  HEAD [-n] <f> / WC <f> / CP|MV <src> <dst>\n"
      "  DEL|RM <f> / MKDIR <d> / RMDIR <d> / TOUCH <f>\n"
      "  ATTRIB <f> [+r|-r][+h|-h][+s|-s][+a|-a]\n"
      "  DIR /A shows hidden/system. \\GT-DOS\\ is protected.\n"
      "\n"
      "Edit: arrows/Home/End/Del, Up/Down=history.\n"
      "Scroll: wheel / Ctrl+Up/Down / PgUp/PgDn.\n"
      "Keys: Ctrl+C cancel, NumLock keypad, CapsLock.\n",
      "GT-DOS 命令一览:\n"
      "  HELP VER CLS ECHO <文本>\n"
      "  DATE TIME MEM KERNEL [VERIFY] UPTIME CPUID BEEP\n"
      "  SERIAL ON|OFF  串口开关\n"
      "  COLOR 0-15     文字颜色\n"
      "  DIV0           除零测试\n"
      "  WHOAMI USERS LOGIN LOGOUT|EXIT\n"
      "  PASSWD [用户]  修改口令 (非管理员仅限自己)\n"
      "  USERADD <名> [-a] / USERDEL <名>   [管理员]\n"
      "  HOSTNAME [名]  查看/设置主机名      [设置=管理员]\n"
      "  CONFIG SHOW|RESET / OOBE            [管理员]\n"
      "  APIC ON|OFF / IRQSTAT  中断统计\n"
      "  RUN <文件.gtp>   加载并运行程序      [管理员]\n"
      "  TC <源.c> [-o 输出]  编译 C 子集 -> .gtp\n"
      "  ASM <源.s> [-o 输出] 汇编 x86-64 源码 -> .gtp\n"
      "  SDK: \\SDK\\TC.H  HELLO.C  FIB.C  NET.H\n"
      "  TASKS|PS / THREADS  任务与线程表\n"
      "  DESKTOP|GUI / TEXTMODE  图形桌面与鼠标指针\n"
      "  SHOW <png|ico|svg> / PNGINFO <png>   查看图片 / 显示文件头\n"
      "  SERVICE LIST|RUN <名字>  SYS 任务受保护\n"
      "  NET / DHCP / PING <ip> / TCP <ip> <端口> [文本]\n"
      "  UDP <ip> <端口> [字节数] / udp_recvfrom 返回 ICMP 差错\n"
      "  DNS <域名> / TFTP GET <ip[:端口]> <文件> [本地]\n"
      "  HTTP GET <主机[:端口]> <路径> [本地]\n"
      "  REBOOT / SHUTDOWN  重启/关机        [管理员]\n"
      "  (Ctrl+Alt+Del 紧急重启)\n"
      "磁盘 (exFAT):\n"
      "  DISK MOUNT DF DRIVES\n"
      "  DIR|LS [/A] [路径] / CD [目录] / PWD / TREE\n"
      "  TYPE <文件> / CAT [-n] <文件...> / RICH <文件.md> / VIM <文件>\n"
      "  PUT <文件> <文本> / APPEND <文件> <一行>\n"
      "  HEAD [-n] <文件> / WC <文件> / CP|MV <源> <目标>\n"
      "  DEL|RM <文件> / MKDIR <目录> / RMDIR <目录> / TOUCH <文件>\n"
      "  ATTRIB <文件> [+r|-r][+h|-h][+s|-s][+a|-a]\n"
      "  DIR /A 显示隐藏/系统文件。\\GT-DOS\\ 受保护。\n"
      "\n"
      "编辑: 方向键/Home/End/Del, 上/下=历史。\n"
      "滚动: 鼠标滚轮 / Ctrl+上/下 / PgUp/PgDn。\n"
      "按键: Ctrl+C 取消, NumLock 小键盘, CapsLock。\n" },

    /* ---- GUI 桌面 / 应用 ---- */
    { "gui.desktop",      "GT-DOS Desktop", "GT-DOS 桌面" },
    { "gui.hint",         "drag title bar | click to capture mouse",
      "拖动标题栏 | 点击捕获鼠标" },
    { "gui.term",         "Terminal", "终端" },
    { "gui.files",        "Files", "文件" },
    { "gui.sys",          "System", "系统" },
    { "gui.start",        "Start", "开始" },
    { "gui.text",         "TEXT", "文本" },
    { "gui.terminal",     "GT-DOS Terminal", "GT-DOS 终端" },
    { "gui.menu.terminal","Terminal", "终端" },
    { "gui.menu.theme",   "Theme", "主题" },
    { "gui.menu.about",   "About", "关于" },
    { "gui.menu.settings","Settings", "设置" },
    { "gui.menu.widgets", "Widgets", "控件" },
    { "gui.menu.exit",    "Exit GUI", "退出图形界面" },
    { "gui.about_title",  "About GT-DOS", "关于 GT-DOS" },
    { "gui.ok",           "OK", "确定" },
    { "gui.settings_title","Settings", "设置" },
    { "gui.theme",        "Theme:", "主题:" },
    { "gui.blue",         "Blue", "蓝" },
    { "gui.green",        "Green", "绿" },
    { "gui.purple",       "Purple", "紫" },
    { "gui.icons",        "Desktop icons", "桌面图标" },
    { "gui.clock24",      "24-hour clock", "24 小时制" },
    { "gui.brightness",   "Brightness", "亮度" },
    { "gui.close",        "Close", "关闭" },
    { "gui.widgets_title","Widgets", "控件演示" },
    { "gui.progress",     "Progress", "进度" },
    { "gui.taskbar_clock","Taskbar clock", "任务栏时钟" },
    { "gui.lang",         "Language", "语言" },
    { "gui.about_l1",     "GT-DOS 0.10  (x86-64 kernel)",
      "GT-DOS 0.10  (x86-64 内核)" },
    { "gui.about_l2",     "Linux-style desktop: panel, icons, window",
      "Linux 风格桌面: 面板 / 图标 / 窗口" },
    { "gui.about_l3",     "Mouse: PS/2, captured on hover (Ctrl+Alt+G)",
      "鼠标: PS/2, 移入捕获 (Ctrl+Alt+G)" },
    { "gui.about_l4",     "Drag title bar to move, double-click to maximize",
      "拖动标题栏移动, 双击最大化" },
    { "gui.about_l5",     "Panel -> Start / Theme / About / Settings / Widgets",
      "面板 -> 开始 / 主题 / 关于 / 设置 / 控件" },
    { "gui.about_l6",     "Click TEXT or the window close button to exit GUI",
      "点击 TEXT 或窗口关闭按钮退出图形界面" },

    /* 右键菜单 / 属性 / 应用窗口 */
    { "gui.ctx.terminal", "Open Terminal", "打开终端" },
    { "gui.ctx.files",    "Open Files", "打开文件" },
    { "gui.ctx.sys",      "System Info", "系统信息" },
    { "gui.ctx.settings", "Settings", "设置" },
    { "gui.ctx.props",    "Properties", "属性" },
    { "gui.ctx.open",     "Open", "打开" },
    { "gui.props_title",  "Properties", "属性" },
    { "gui.prop.name",    "Name", "名称" },
    { "gui.prop.type",    "Type", "类型" },
    { "gui.prop.size",    "Size", "大小" },
    { "gui.prop.loc",     "Location", "位置" },
    { "gui.prop.res",     "Resolution", "分辨率" },
    { "gui.prop.uptime",  "Uptime", "运行时间" },
    { "gui.prop.theme",   "Theme", "主题" },
    { "gui.prop.disk",    "Disk free", "磁盘可用" },
    { "gui.prop.lang",    "Language", "语言" },
    { "gui.prop.ver",     "Version", "版本" },
    { "gui.desk",         "Desktop", "桌面" },
    { "gui.desk.c",       "C:\\", "C:\\" },
    { "gui.dir",          "<DIR>", "<目录>" },
    { "gui.empty",        "(empty)", "(空)" },
    { "gui.files_title",  "Files", "文件" },
    { "gui.sys_title",    "System", "系统" },
    { "gui.app",          "Application", "应用程序" },
    { "gui.folder",       "Folder", "文件夹" },
    { "gui.kernel",       "Kernel", "内核" },
    { "gui.uptime_s",     "seconds", "秒" },
    { "gui.bytes",        "bytes", "字节" },
    { "gui.clusters",     "clusters", "簇" },

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
