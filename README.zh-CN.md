# GT-DOS

[English](README.md) | [简体中文](README.zh-CN.md)

GT-DOS 是一个从零实现的 **x64 教学操作系统**，使用 NASM 汇编与 C（clang/LLVM 编译）编写。
它通过自己的 MBR 与二级加载器，从单个 64 MB 硬盘镜像引导进入 64 位长模式，并提供文本控制台、
exFAT 文件系统、TCP/IP 网络栈、用户账户与中英双语 Shell。

![version](https://img.shields.io/badge/version-0.10-blue)

## 功能特性

**引导**
- 自实现 MBR（`boot/mbr.asm`），分区表由构建脚本写入
- 二级加载器（`boot/boot.asm`），以 ATA LBA 读取（AH=42h / DAP）加载内核
- 内核加载至 `0x10000`，硬性上限 256 KB
- 内核以受保护容器形式提供：`GTK1` 尾部记录长度、CRC32 与 SHA-256，
  第一条 C 语句在任何其它代码之前校验载入镜像
- 内核代码与只读数据用 4KB 页映射并清除 RW 位，内核态同样不可写；
  空闲看门狗每 5 秒按 `GTR1` CRC32 复核一次，不一致立即停机

**内核**
- 16 位实模式 → 32 位保护模式 → **x64 长模式**，全部在 `kernel/entry.asm` 中完成
- 在 32 位模式下构建恒等页表（PML4 + PDPT + 2 MB 大页）
- 完整映射 1 GB 不可缓存 MMIO 窗口，保证帧缓冲可访问
- 16 字节 x64 门描述符的 IDT、中断桩、符合 SysV 约定的寄存器保存/恢复
- APIC + IO-APIC，具备中断优先级（TPR）与向量分级
- 软中断模式：上半部只负责缓存输入，下半部在空闲循环中解码
- ATA PIO 驱动、exFAT 文件系统（长文件名、分配位图、FAT 链）、串口控制台（UTF-8）

**图形**
- Bochs VBE 线性帧缓冲，LFB 地址与显存容量通过 PCI 配置空间探测获得
- 分辨率自适配：优先 1920x1080，显存或模式不支持时依次回退 1024x768 / 800x600 / 640x480
- 窗口模式会把分辨率限制在宿主桌面可用区域内（`build.py` 写入内核），避免 QEMU 窗口被
  任务栏或屏幕边缘裁切；`run -full` 取消该限制，由 QEMU 缩放显示
- 字模整数倍缩放（1x–3x），自动选取让文本网格铺满屏幕的倍率
  （例如 1920x1080 下为 2 倍字模、120x33 字符单元）
- 双缓冲——所有绘制先写入后台缓冲再翻转，因此无闪烁
- 文本控制台在帧缓冲上软渲染，中文不受 VGA 256 槽限制

**字体**
- 16x16 灰度字模，以原生 16 px、GDI `CLEARTYPE_QUALITY` 渲染，填满整个单元
- 每像素 4 bit、16 级灰度，绘制时与背景做 alpha 混合
- 约 21,400 个字模（ASCII、拉丁字母、符号与中文）共用自适应基线，
  因此中英文可以整齐排列在同一行
- 由 Rust 工具 `rust/mkfont` 生成 `build/font.gtd`（2.75 MB）与 `cpm.gtd`

**系统**
- OOBE 首次启动向导（语言 → 主机名 → 管理员）
- 带口令的用户账户；`admin` 拥有管理员权限
- i18n 层，提供完整的中英文文案

**系统调用与程序**
- `INT 0x21` 软中断门：`rax` 选择调用号，`rdi`/`rsi`/`rdx` 依次传参（SysV 约定），
  返回值写回 `rax`：

| 号 | 调用 | 号 | 调用 |
| --- | --- | --- | --- |
| 0 | `SYS_EXIT` | 10 | `SYS_GETTIME` |
| 1 | `SYS_PUTC` | 11 | `SYS_PUTS` |
| 2 | `SYS_GETC` | 12 | `SYS_SEEK` |
| 3 | `SYS_WRITE` | 13 | `SYS_STAT` |
| 4 | `SYS_READ` | 14 | `SYS_OPENDIR` |
| 5 | `SYS_OPEN` | 15 | `SYS_READDIR` |
| 6 | `SYS_CLOSE` | 16 | `SYS_SLEEP` |
| 7 | `SYS_UPTIME` | 17 | `SYS_LIBLOAD` |
| 8 | `SYS_REBOOT` | 18 | `SYS_LIBSYM` |
| 9 | `SYS_SHUTDOWN` | 19 | `SYS_STATX` |
| | | 20 | `SYS_IPRINT` |
| | | 21 | `SYS_NETINFO` |
| | | 22 | `SYS_TCP_CONNECT` |
| | | 23 | `SYS_TCP_SEND` |
| | | 24 | `SYS_TCP_RECV` |
| | | 25 | `SYS_TCP_CLOSE` |
| | | 26 | `SYS_TCP_LISTEN` |
| | | 27 | `SYS_TCP_ACCEPT` |
| | | 28 | `SYS_DHCP` |
| | | 29 | `SYS_DNS` |
| | | 30 | `SYS_TFTP_GET` |
| | | 31 | `SYS_HTTP_GET` |
| | | 32 | `SYS_UDP_OPEN` |
| | | 33 | `SYS_UDP_SENDTO` |
| | | 34 | `SYS_UDP_RECVFROM` |
| | | 35 | `SYS_ICMP_ERR` |
| | | 36 | `SYS_UDP_CLOSE` |
| | | 37 | `SYS_PRINTF` |
| 38 | `SYS_CLS` | 43 | `SYS_RMDIR` |
| 39 | `SYS_COLOR` | 44 | `SYS_GETCWD` |
| 40 | `SYS_SCREEN` | 45 | `SYS_HOSTNAME` |
| 41 | `SYS_UNLINK` | 46 | `SYS_USERNAME` |
| 42 | `SYS_MKDIR` | 47 | `SYS_ISADMIN` |
| | | 48 | `SYS_VERSION` |
| | | 49 | `SYS_UNIXTIME` |
| | | 50 | `SYS_RICH` |

- 套接字调用最多使用六个参数（`rdi`、`rsi`、`rdx`、`rcx`、`r8`、`r9`）；
  前三项与原 ABI 相同，既有程序不受影响

- 基于 exFAT 的 `open/read/write/close/seek` fd 层（fd 0/1/2 为控制台），
  目录句柄经 `SYS_OPENDIR`/`SYS_READDIR` 遍历
- `RUN <文件.gtp>` 将平面二进制载入暂存区并跳转执行；程序经 `SYS_EXIT` 返回 shell
- `.gtl` 共享库：加载器把镜像映射到 `LIB_BASE`（0x580000），
  由 `SYS_LIBLOAD`/`SYS_LIBSYM` 按名解析导出符号；每个程序运行前重置库命名空间
- `.gtd` 数据文件通过同一 fd 层读取（示例程序读取 `DATA.GTD`）
- `kernel/include/` 下的公开头文件（`gtos.h`、`unistd.h`、`stat.h`、`dirent.h`、`gtlib.h`、
  `time.h`、`stdint.h`、`ctype.h`、`stdio.h`、`string.h`、`stdlib.h`）
  被种子写入磁盘的 `INCLUDE` 目录

**网络**
- RTL8139 PCI 网卡驱动，轮询式收/发环（不依赖网卡中断）
- Ethernet II + ARP 缓存；IPv4 头部校验；发送时自动分片、接收时重组分片
  （2 个重组槽，单个报文最大 8 KB）
- ICMP echo（`PING`）与 ICMP 差错处理：目的不可达、超时、参数错误会被记录，
  并让 `udp_recvfrom` 返回 `-2` 或使 TCP 收发失败（用户程序可用 `SYS_ICMP_ERR`
  取回完整差错记录）
- UDP 与 DHCP 客户端（`NET` / `DHCP`）；显示链路、MAC、IP、掩码、网关与 DNS
- TCP：主动/被动连接、MSS 选项、序号/确认、接收窗口、乱序段排队重组、
  三次重复 ACK 快速重传、慢启动/拥塞避免/快速恢复、RST 收发，
  以及 FIN 四次挥手（`FIN_WAIT_1/2`、`CLOSE_WAIT`、`LAST_ACK`、`CLOSING`）
- 系统调用 21–36，以及 TC 内置 `ip4`、`tcp_*`、`udp_*`、`icmp_err`；
  Shell 客户端 `TCP <ip> <端口> [文本]` 与 `UDP <ip> <端口> [字节数]`
- UDP socket 表与应用协议：DNS A 记录解析（含名称压缩指针）、TFTP 客户端
  （`RRQ`/`DATA`/`ACK` 与超时重传）、HTTP/1.0 `GET` 下载；下载内容写入 exFAT
- Shell 文件传输：`TFTP GET <ip[:端口]> <远程> [本地]` 与
  `HTTP GET <主机[:端口]> <路径> [本地]`
- QEMU 用户网络默认地址 `10.0.2.15/24`，网关 `10.0.2.2`

**内置 C 编译器 (TC) 与 SDK**
- `TC <源.c> [更多.c ...] [-o 输出.gtp]` 直接把 C 子集编译为 x86-64 机器码，
  生成可由 `RUN` 执行的 `.gtp` 程序，全程不依赖外部工具链
- 多模块**自动扁平化**：可一次编译多个源文件，按给出顺序合并为一个程序，
  共享符号表、宏与 include 去重表，函数调用在全部模块解析后统一解析，
  每个模块仍以自己的目录解析 `#include "..."`
- 编译优化：`-O0`..`-O4`（`-Os`、`-Ofast`），并可按 pass 覆盖：
  `-fconst/-fno-const`、`-fstrength/-fno-strength`、`-fdce/-fno-dce`、
  `-floop/-fno-loop`，另有 `-g`（调试，重置为 O0）与 `-v`（打印生效选项及
  code/data 大小）。参数联动：`-fdce` 隐含折叠，`-floop` 隐含 `-fdce`，
  `-fno-const` 关闭全部 pass，后出现的参数覆盖先前参数。各层含义：O1 常量折叠 +
  立即数，O2 强度削弱（移位、无符号 2 的幂除法/取模），O3 常量 `if` 死分支消除，
  O4 常量循环消除（`while(0)`、`while(1)`、`do..while(0/1)`）。所有级别都使用
  局部变量直接访问（`mov [rbp+d]`，不再先 `lea` 再走栈）、最短立即数编码
  （`xor`/`imm32`/`imm8` 形式），条件表达式直接生成 `cmp` + `jcc` 而不物化 0/1；
  二元运算/赋值/自增/下标的临时值放在 `r11`/`r10`（常量右操作数延迟保存，
  寄存器不足才落栈），`for` 步进的 `i++` 直接读改写内存；
  基准代码段为 2091 字节（-O0）与 1846 字节（-O4）
- 支持：`int`/`char`/`double`/指针、1–4 维数组、`struct`/`union`（字段、嵌套结构、
  `.` 与 `->`、位域）、函数指针及函数指针数组、函数（最多 6 参数、递归、原型、
  用户可变参数 `...` 与 `va_list`/`va_start`/`va_arg`/`va_end`）、`if/else`、
  `while`、`do/while`、`for`、`switch/case/default`（可穿透）、
  `break`/`continue`/`return`/`goto` 与标签、完整表达式（有/无符号整数与
  `double` 运算/比较）与 `?:`、前置/后置 `++`/`--`、复合赋值、`typedef`、`enum`、
  `sizeof(类型)`/`sizeof 表达式`、强制类型转换、含转义（`\n \t \xNN \NNN`）的
  字符串/字符字面量、全局/局部花括号与字符串初始化
- 扫描器支持对象式与函数式 `#define` 宏（含 `#` 字符串化与 `##` 记号拼接）；
  不实现 `#if`（GT-DOS 只有单一平台），其余指令整行跳过。`#include <...>` /
  `#include "..."` 在编译期展开（依次查找源文件目录、`\SDK`、`\GT-DOS\INCLUDE`），
  每个文件只包含一次；标准库模块（string/stdlib/ctype/stdio/net）**只有对应头文件
  被 include 时才编入程序**，未使用的库不会增大 `out.gtp`
- 网络库：`NET.H`/`NET.C`（按需编入）在 TCP/UDP 内置函数之上提供 `net_aton`、
  `net_resolve`（点分十进制或 DNS）、`net_connect`、`net_send_str`、
  `net_send_all`、`net_recv_line`、`net_recv_all`
- 标准库：`strlen strcpy strncpy strcmp strncmp strcat strncat strchr strrchr strstr`
  `memcpy memmove memset memcmp`、`malloc calloc free strdup abs atoi strtol rand srand`
  （16KB 堆）、
  `isdigit isalpha isalnum isspace isupper islower toupper tolower`、
  `printf putchar puts getchar`；`printf` 由编译器生成 SYS_PRINTF，
  支持 `%d %i %u %x %X %c %s %p %f %%`，最多 5 个可变参数
- 不支持：`long double`、返回函数指针的函数，以及 `#if` 条件编译
- 内置函数（`\SDK\TC.H`）：`putc`、`print`、`print_int`、`getc`、`exit`、
  `sleep`、`uptime`、`open`、`read`、`write`、`close`、`seek`、`stat`、`statx`、
  `reboot`、`shutdown`；控制台/系统 `cls`、`color`、`screen`、`getcwd`、`hostname`、
  `username`、`isadmin`、`version`、`unixtime`、`rich`；文件系统 `unlink`、`mkdir`、
  `rmdir`；`\SDK\NET.H` 另含 `ip4`、`tcp_connect`、`tcp_send`、
  `tcp_recv`、`tcp_close`、`dns`、`tftp_get`、`http_get`
- `\SDK\` 提供明文只读 SDK：`TC.H`、`STRING.H`、`STDLIB.H`、`CTYPE.H`、`STDIO.H`、
  `STDARG.H`、`NET.H`、
  `STRING.C`、`STDLIB.C`、`CTYPE.C`、`STDIO.C`、`NET.C`、`DEMO.MD`、`HELLO.C`、
  `HELLO.S`、`FIB.C`、`README.TXT`；
  配合 `PUT` 与 `APPEND` 可在系统内逐行编写源码
- 程序布局：代码 128KB 位于 `0x600000`，数据 128KB 位于 `0x620000`
  （全局 64KB + 字符串池 64KB）

**内置汇编器 (ASM)**
- `ASM <源.s> [-o 输出.gtp]` 把 NASM 风格的单文件 x86-64 汇编直接编码为 `.gtp`，
  与 TC 使用相同的程序布局（代码 `0x600000`、数据 `0x620000`），可直接 `RUN`
- 语法：`.text`/`.data`（或 `section .text`/`.data`）、标签、`equ` 常量、
  `db/dw/dd/dq`、`times`；64 位寄存器 `rax..r15` 与 32 位 `eax..r15d`；
  内存操作数 `[reg]`、`[reg+disp]`、`[reg+index*scale+disp]`、`[label]`、`[disp]`
- 指令：`mov lea add sub and or xor cmp test inc dec neg not imul div idiv
  shl shr sar push pop call ret jmp jcc int nop hlt leave cqo cdq`；
  支持 `je/jne/jl/jle/jg/jge/jb/jbe/ja/jae/js/jns` 等条件跳转（rel32）
- 入口为代码偏移 0；通过 `INT 0x21`（`rax` = 调用号）访问内核，
  `rdi`=退出码、`rax=0` 调用 `SYS_EXIT`；`\SDK\HELLO.S` 为示例

**富文本与编辑器**
- `CAT [-n] <文件...>` 依次显示多个文件；`-n` 连续编号
- `RICH <文件.md>` 渲染 Markdown：ATX/Setext 标题、粗体/斜体/粗斜体/删除线、
  行内代码、链接与图片、嵌套列表（无序/有序/任务）、多级引用、分隔线、带
  C 系语言简易高亮的围栏代码块，以及自动列宽并按 `:---:`/`---:` 对齐的表格；
  同时支持 `<b> <i> <c=red> </c> <br>` 内联标签
- 程序可通过 `SYS_RICH`（TC 内建 `rich`）直接渲染 Markdown，并用 `SYS_SCREEN`
  读取终端尺寸
- `VIM <文件>` 全屏模态编辑器（normal/insert/command/search）：方向键、
  Home/End/PgUp/PgDn、`i I a A o O`、`x X`、`dd`、`yy`、`p`、一级 `u`、`gg/G`、
  `w/b` 按词移动、`J` 合并行、`D` 删至行尾、Ctrl+D/Ctrl+U 半屏滚动、
  `/文本` + `n`、`:w :q :q! :wq`，带行号与状态栏，最大 256KB，退出后恢复控制台
- `.c/.h/.cpp` 文件自动启用 C 语法高亮（预处理、注释、字符串、字符、数字、
  类型与关键字），支持水平滚动与制表符对齐
- `\SDK\DEMO.MD` 提供富文本示例

**exFAT 厂商扩展**
- 每个文件/目录的目录项集附带 3 个厂商扩展项（类型 `0xE0`）：
  加密元数据（标志、算法、明文长度、CRC32）、ACL（所有者与权限位）、版本与 CRC32
- `SYS_STATX` 把它们暴露给程序（`flags`/`version`/`crc`/`owner`/`mode`）；
  非管理员写入时内核按所有者校验

**安全**
- `\GT-DOS\` 下所有文件以 AES-128-CBC（PKCS#7 填充）密文存储；内核内部读取时透明解密
- 内核即受保护程序：`.container` 尾部（`GTK1`）保存长度、CRC32 与 SHA-256，
  `kernel_self_check()` 在任何其它代码之前校验载入镜像；`KERNEL` 命令显示校验结果，
  并与加密的 `\GT-DOS\KERNEL.GTP` 视图比对；`RUN` 拒绝执行该镜像
- 运行时防篡改：代码/只读数据区间为 4KB 只读页（CR0.WP 已置位），
  空闲看门狗每 5 秒复核，`KERNEL VERIFY` 可手动复核；不一致立即停机
- 每个 `.gtp` 程序在跳转前都会与其 exFAT 厂商元数据中的明文 CRC32 比对，
  被篡改或损坏的程序会被拒绝
- 格式解析器位于 Rust 侧（`rust/gtcore`：`parse.rs`、`sha256.rs`）：
  `.gtl` 头部与导出表、`.gtd` 字模查找、`.gtp` 完整性、exFAT 目录项；
  C 侧只保留内存映射与跳转
- 加密与 CRC32 由 Rust 实现（`rust/gtcore`，`no_std`）并链接进内核；
  构建期格式化工具 `rust/gtmkfs` 复用同一份实现
- 镜像格式化、目录项与厂商扩展完全由 Rust 工具 `rust/gtmkfs` 完成；
  构建脚本只生成种子清单并调用一次该工具
- 系统文件严格保护：任何账户（包括 `admin`）都不可读、不可改、不可删
  （`TYPE`/`HEAD`/`WC`/`CP`/`MV` 拒绝，写入与删除被拦截）
- 非管理员仅可写自己家目录（`\USERS\<名>\`）内的文件
- 特权命令（`USERADD`、`USERDEL`、`HOSTNAME`、`CONFIG RESET`、`OOBE`、`MOUNT`、`DIV0`、
  `RUN`、`REBOOT`、`SHUTDOWN`）需管理员权限
- 编译器栈保护（`-fstack-protector-strong`）与边界断言（`GT_ASSERT`）

## 快速开始

### 环境要求

| 依赖 | 说明 |
| --- | --- |
| Windows | 构建脚本与字模生成器使用 Win32 API |
| Python 3.8+ | 用于运行 `build.py` |
| Rust (stable) | `cargo` 需在 `PATH`，并安装裸机目标 `x86_64-unknown-none`（`rustup target add x86_64-unknown-none`） |
| NASM | `nasm.exe` 需在 `PATH`，或位于 `C:\Program Files\NASM` |
| LLVM / clang | `clang.exe`、`ld.lld.exe`、`llvm-objcopy.exe` 需在 `D:\LLVM\bin` 或 `C:\Program Files\LLVM\bin` |
| QEMU | `qemu-system-x86_64.exe` 需在 `qemu/` 目录或 `PATH` 中 |

`build.py` 会在 `CANDIDATES` 表中列出的位置查找工具链；若你的工具链在别处，请修改该表。

### 编译与运行

```sh
python build.py            # 编译, 生成 build/gt-dos-hd.img
python build.py run        # 用 QEMU 启动镜像
python build.py run -nogui # 仅串口输出, 不开图形窗口
python build.py run -full  # 全屏窗口 (分辨率由内核自适配)
python build.py run -debug # 启动并开启 CPU/中断跟踪日志
python build.py all        # 先编译再运行
python build.py clean      # 清理 build 目录
```

首次编译会一并生成字模：当 `build/font.gtd` 或 `build/cpm.gtd` 缺失时，
会自动构建并调用 Rust 工具 `rust/mkfont`。内核各 C/汇编编译单元相互独立，
`build.py` 会按 CPU 核数并行编译后统一链接，缩短整体构建时间。

直接启动已发布的镜像：

```sh
qemu-system-x86_64 -hda gt-dos-hd.img -boot c
```

首次启动会进入 OOBE 向导，依次询问语言、主机名与管理员名。
输入为空会重新询问；`Ctrl-C` 则以默认值跳过当前项。

## 磁盘布局

镜像为单个 64 MB 原始硬盘，带 MBR 分区表：

| 区域 | 位置 | 内容 |
| --- | --- | --- |
| MBR | LBA 0 | `boot/mbr.asm`，分区表由 `build.py` 写入 |
| 分区 1（C:） | LBA 2048，120832 扇区 | exFAT 系统盘，含 `\GT-DOS\` 目录与 `INCLUDE` 头文件 |
| 分区 2（隐藏） | 盘尾 8192 扇区 | 启动分区，类型 `0x11`，带可引导标志：`boot.gtb` + `kernel.gtb` + `font.gtd` + `cpm.gtd` |

启动分区位于磁盘末尾并携带可引导标志；内核在启动时通过 ATA 从中读取 `font.gtd` 与 `cpm.gtd`。

自研二进制采用分类语义后缀：`.gtb` 为引导/内核裸二进制（`mbr`/`boot`/`kernel`），`.gtd` 为数据块
（`font`/`cpm`/`data`），`.gtp` 为用户程序（`hello`/`demo`），`.gtl` 为共享库（`sample_lib`）。

## Shell 命令

在 GT-DOS 中输入 `HELP`（或 `?`）可查看完整列表。命令同时支持长名与短名。

**系统** — `HELP`/`?`、`VER`、`CLS`、`DATE`、`TIME`、`MEM`、`KERNEL`、`CPUID`、`UPTIME`、
`BEEP`、`COLOR`、`SERIAL [OFF]`

**用户** — `WHOAMI`、`USERS`、`LOGIN`、`LOGOUT`/`EXIT`、`PASSWD`、`USERADD`、`USERDEL`、
`HOSTNAME`、`CONFIG`、`OOBE`（`USERADD`、`USERDEL`、`HOSTNAME`、`SHUTDOWN`、`REBOOT`
需要管理员权限）

**文件与磁盘** — `CD`、`PWD`、`DRIVES`、`DIR`/`LS [/A]`、`TYPE`、`CAT [-n] <文件...>`、
`RICH <文件.md>`、`VIM <文件>`、`PUT`、`DEL`/`RM`、
`MKDIR`/`MD`、`RMDIR`、`TOUCH`、`ATTRIB`、`CP`/`COPY`、`MV`/`REN`、`HEAD`、`WC`、`TREE`、
`DF`、`DISK`、`MOUNT`、`APPEND`

**显示与硬件** — `APIC`、`IRQSTAT`

**网络** — `NET`、`DHCP`、`PING <ip> [次数]`、`TCP <ip> <端口> [文本]`、
`UDP <ip> <端口> [字节数]`、`DNS <域名>`、`TFTP GET <ip[:端口]> <远程> [本地]`、
`HTTP GET <主机[:端口]> <路径> [本地]`

**程序** — `RUN <文件.gtp>`（需要管理员权限）、`TC <源.c> [更多.c ...] [-o 输出.gtp]`（C 子集编译器，多模块扁平化）、`ASM <源.s> [-o 输出.gtp]`（x86-64 汇编器）

**任务与服务** — `TASKS`/`PS`、`SERVICE LIST`、`SERVICE RUN <名字>`、`SERVICE STOP <名字>`

**电源** — `REBOOT`、`SHUTDOWN`/`POWEROFF`（需要管理员权限）

## 键盘

PS/2 扫描码集 1 驱动，支持 E0 扩展键：方向键、Home/End、PgUp/PgDn、Ins/Del。
Ctrl 组合键转换为 ASCII 控制码：`Ctrl+C` 取消当前行，`Ctrl+A`/`Ctrl+E` 跳到行首/行尾，
`Ctrl+P`/`Ctrl+N` 翻阅历史；`Ctrl+上/下` 滚动控制台，`Ctrl+Alt+Del` 重启。
`NumLock` 默认开启，开箱即用小键盘输入数字/运算符；关闭后变为导航键
（Home/End/PgUp/PgDn/Ins/Del）。小键盘 `Enter` 与 `+ - * / .` 均可使用，
CapsLock 与 NumLock 指示灯随驱动状态更新。

## 任务与系统服务

- 内核子系统（`kernel.core`、`kernel.image`、`net.stack`、`fs.exfat`、`tty.console`、
  `crypto.aes`）在启动时登记为 SYS 任务；`TASKS`/`PS` 显示编号、类型（SYS/USR）、
  保护标记、名称、状态、内存与所有者。
- `kernel.image` 代表 `GTK1` 容器；`KERNEL` 显示其校验后的大小、CRC32、SHA-256，
  以及与 `\GT-DOS\KERNEL.GTP`（加密视图）是否一致。
- 系统服务是 `\GT-DOS\SERVICES\` 下的普通 `.gtp` 程序（构建期加密写入，例如 `NETSVC.GTP`）。
  `SERVICE RUN <名字>` 运行它；由于程序位于 `\GT-DOS\`，运行期间会登记为 SYS 任务。
- SYS 任务与服务文件受保护：`SERVICE STOP` 拒绝，`DEL`/`ATTRIB`/`CP` 针对 `\GT-DOS\`
  路径一律拦截。

## 项目结构

```
boot/
  mbr.asm            MBR, 负责加载二级加载器
  boot.asm           二级加载器, 加载内核并传递元数据
kernel/
  entry.asm          实模式 -> 保护模式 -> 长模式, 页表构建
  isr.asm            中断桩, INT 0x21 系统调用桩, 公共寄存器帧
  exec_asm.asm       RUN 的栈帧切换 (exec_enter / exec_longjmp)
  kernel.ld          链接脚本; 大缓冲位于 1MB 以上的 .highbss
  gt.h               内核全局定义
  include/           公开头文件 (gtos.h, unistd.h, stat.h, dirent.h, gtlib.h 等)
  kmain.c            内核入口
  idt.c apic.c       中断: IDT, APIC/IO-APIC, 分发
  syscall.c          INT 0x21 系统调用分派
  exec.c             RUN 程序加载器 (把 .gtp 载入 PROG_BASE, 校验 CRC)
  protect.c          GTK1 内核容器、4KB 写保护、自检与看门狗
  gtlib.c            .gtl 共享库加载器 (头部/导出表由 Rust 解析)
  tc.c               内置 C 编译器 (数组/结构体/typedef/#include, x86-64)
  richtext.c         Markdown/富文本渲染 (CAT/RICH)
  vim.c              全屏模态编辑器 (VIM)
  net.c              RTL8139 驱动 + ARP/IPv4/ICMP/UDP/DHCP
  tcp.c              TCP socket (握手/收发/重传/挥手)
  netapp.c           DNS 解析、TFTP 客户端、HTTP GET 下载
  task.c             系统任务表 (SYS/USR) 与 SYS 保护
  timer.c keyboard.c mouse.c serial.c    设备驱动
  softirq.c          输入下半部的延迟处理
  ata.c fat.c cfg.c user.c               存储, exFAT 驱动(含 fd 层), 配置, 账户
  gfx.c vga.c tui.c console.c ui.c       图形层, 文本控制台, 启动画面
  shell.c            命令解释器
  i18n.c power.c oobe.c lib.c            文案, 复位/关机, 首次启动向导, libc 桩
rust/
  gtcore/            no_std AES-128-CBC + CRC32 + SHA-256 与格式解析器
                     (.gtl/.gtd/.gtp/exFAT), 同时链接进内核与 gtmkfs
  gtmkfs/            exFAT 格式化/填充 + 加密种子文件 (构建期工具)
  mkfont/            GDI 字模生成器 (4bit 灰度 font.gtd + cpm.gtd)
tools/
  sdk/               系统内 SDK: tc.h, net.h, hello.c, fib.c, net.c, readme.txt
  hello.asm          示例程序 (INT 0x21 SYS_WRITE/SYS_EXIT), 种子为 HELLO.GTP
  demo.asm           示例程序: .gtd 读取/seek、stat、.gtl 库调用 (DEMO.GTP)
  sample_lib.asm     示例共享库, 导出 add/mul (SAMPLE.GTL)
  svc_net.asm        系统服务程序, 种子为 \GT-DOS\SERVICES\NETSVC.GTP
  test_v03.py        端到端测试: OOBE, 登录, 文件命令
  test_v04.py        回归测试: 单盘 C:, GT-DOS 目录, 属性, 系统保护
  test_syscall.py    INT 0x21 门与 RUN 程序执行测试
  test_net.py        RTL8139/ARP/ICMP/DHCP/TCP/UDP(分片)/TC 测试（宿主回显服务器）
  test_tasks.py      系统任务表、SYS 服务与保护测试
  test_protect.py    内核容器与 .gtp 篡改检测测试
  test_kbd.py        PS/2 键盘测试 (NumLock、Ctrl 组合、Ctrl+Alt+Del)
  sdk/bench.c        TC 微基准 (循环/调用/double/数组/递归)
  bench_host.rs      同一组负载的宿主原生基线 (rustc -O)
  test_perf.py       TC 与原生性能对比并生成报告
build.py             构建脚本与镜像组装
```

测试脚本通过串口 socket 与 QEMU monitor socket 驱动 QEMU 实例，
因此它们校验的是真实启动输出，而非模拟数据。

`tools/test_perf.py` 分别在 `-O0` 与 `-O4` 下编译并运行 `\SDK\BENCH.C`，
同时用 `rustc -O` 在宿主运行同一组负载，生成 `build/perf_report.md`。
测试机上基准代码段为 2091 字节（-O0）与 1846 字节（-O4）；整数循环约 50 ms、
常量表达式循环在 -O4 下约快 1.2–1.6×（70→50 ms）、数组索引约 60 ms（均为 5M 次），
计时精度 10 ms，存在采样噪声。
整体仍比原生慢 14–150×（随负载变化），主要来自 QEMU TCG 模拟以及 TC 简单的
代码生成（所有值经栈槽、不做寄存器分配）。

## 说明与限制

- 内核目前为 545 扇区（278,848 字节，含 48 字节 `GTK1` 尾部），构建上限 640 扇区
  （320KB），加载区间为 `0x10000` 到 `0x60000`；链接脚本同时断言
  `.bss <= 0xA0000` 与 `.highbss <= LIB_BASE`。
- GDT 在低端内存（0x6800）由入口代码构造，而非放在 `.data`：CPU 装载段寄存器
  会置位描述符的 accessed 位，否则会破坏内核哈希。
- 启动哈希覆盖完整镜像（代码 + 只读数据 + 数据段）；运行时看门狗只覆盖不可变的
  `[0x10000, __ro_end)`，因为 `.data` 在运行中会合法变化。
- 大块零初始化缓冲（`font_buf`、码点表、后台缓冲）被放置在 1 MB 以上、
  标记为 `NOLOAD` 的 `.highbss` 段。若放在普通 `.bss` 中，会与 `0xA0000`
  处的 VGA 显存孔径重叠并破坏帧缓冲。
- TC 已支持数组、结构体/联合（含位域）、`switch`、`?:`、`++/--`、`typedef`/`enum`/
  `sizeof`、强制转换、函数指针、用户可变参数函数与 `double` 运算；`#define` 宏与
  `#include` 在编译期展开，并按需链接对应标准库模块。`long double` 与 `#if` 不支持。
  程序代码上限 128KB，数据上限 128KB（全局 64KB + 字符串 64KB）。
- 内核 `.bss` 必须保持在 `0xA0000`（VGA 孔径）以下，`.highbss` 必须低于 `LIB_BASE`；
  链接脚本对两者都有断言，表过大时会直接构建失败。
- Shell 文件命令（`TYPE`/`CP`/`MV`/`HEAD`/`WC`/`APPEND`）处理不超过 64KB 的文件。
- 网络栈仅支持 IPv4（无 IPv6），TCP 未实现 SACK 与窗口缩放，
  `TCP_ACCEPT` 一次只处理一个连接；UDP 单个报文上限 8 KB（受重组缓冲限制）。
- TFTP/HTTP 下载先缓存在内存，单个文件上限 64KB。
- 构建过程需要 Windows；但内核本身是完全独立的，不依赖宿主平台。

## 许可证

本项目基于 [MIT 许可证](LICENSE) 发布。Copyright (c) 2026 GotHCL458。

## 相关链接

- 仓库地址：<https://github.com/GotHCL458/gt-dos>
- 发行版（磁盘镜像）：<https://github.com/GotHCL458/gt-dos/releases>