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
    { "shell.banner",    "GT-DOS v0.9\n",
      "GT-DOS v0.9\n" },
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
      "  WHOAMI / USERS / LOGIN / LOGOUT|EXIT\n"
      "  PASSWD [user]     (self only unless admin)\n"
      "  USERADD <n> [-a] / USERDEL <n>      [admin]\n"
      "  HOSTNAME [name]                     [set=admin]\n"
      "  CONFIG SHOW|RESET / OOBE            [admin]\n"
      "  APIC ON|OFF / IRQSTAT  interrupt stats\n"
      "  RUN <file.gtp>        load and run a program [admin]\n"
      "  TC <src.c> [-o out]   compile C subset -> .gtp\n"
      "  SDK: \\SDK\\TC.H  HELLO.C  FIB.C  NET.H\n"
      "  NET / DHCP / PING <ip> / TCP <ip> <port> [text]\n"
      "  REBOOT / SHUTDOWN                   [admin]\n"
      "  (Ctrl+Alt+Del = emergency reboot)\n"
      "Disk (exFAT):\n"
      "  DISK / MOUNT / DF / DRIVES\n"
      "  DIR|LS [/A] [path] / CD [dir] / PWD / TREE\n"
      "  TYPE|CAT <f> / PUT <f> <text> / APPEND <f> <line>\n"
      "  HEAD [-n] <f> / WC <f> / CP|MV <src> <dst>\n"
      "  DEL|RM <f> / MKDIR <d> / RMDIR <d> / TOUCH <f>\n"
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
      "  WHOAMI USERS LOGIN LOGOUT|EXIT\n"
      "  PASSWD [用户]  修改口令 (非管理员仅限自己)\n"
      "  USERADD <名> [-a] / USERDEL <名>   [管理员]\n"
      "  HOSTNAME [名]  查看/设置主机名      [设置=管理员]\n"
      "  CONFIG SHOW|RESET / OOBE            [管理员]\n"
      "  APIC ON|OFF / IRQSTAT  中断统计\n"
      "  RUN <文件.gtp>   加载并运行程序      [管理员]\n"
      "  TC <源.c> [-o 输出]  编译 C 子集 -> .gtp\n"
      "  SDK: \\SDK\\TC.H  HELLO.C  FIB.C  NET.H\n"
      "  NET / DHCP / PING <ip> / TCP <ip> <端口> [文本]\n"
      "  REBOOT / SHUTDOWN  重启/关机        [管理员]\n"
      "  (Ctrl+Alt+Del 紧急重启)\n"
      "磁盘 (exFAT):\n"
      "  DISK MOUNT DF DRIVES\n"
      "  DIR|LS [/A] [路径] / CD [目录] / PWD / TREE\n"
      "  TYPE|CAT <文件> / PUT <文件> <文本> / APPEND <文件> <一行>\n"
      "  HEAD [-n] <文件> / WC <文件> / CP|MV <源> <目标>\n"
      "  DEL|RM <文件> / MKDIR <目录> / RMDIR <目录> / TOUCH <文件>\n"
      "  ATTRIB <文件> [+r|-r][+h|-h][+s|-s][+a|-a]\n"
      "  DIR /A 显示隐藏/系统文件。\\GT-DOS\\ 受保护。\n"
      "\n"
      "编辑: 方向键/Home/End/Del, 上/下=历史。\n"
      "滚动: 鼠标滚轮 / Ctrl+上/下 / PgUp/PgDn。\n" },

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
