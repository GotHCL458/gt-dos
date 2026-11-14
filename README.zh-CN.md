# GT-DOS

[English](README.md) | [简体中文](README.zh-CN.md)

GT-DOS 是一个从零实现的 **x64 教学操作系统**，使用 NASM 汇编与 C（clang/LLVM 编译）编写。
它通过自己的 MBR 与二级加载器，从单个 64 MB 硬盘镜像引导进入 64 位长模式，并提供文本控制台、
FAT16 文件系统、用户账户与中英双语 Shell。

![version](https://img.shields.io/badge/version-0.9-blue)

## 功能特性

**引导**
- 自实现 MBR（`boot/mbr.asm`），分区表由构建脚本写入
- 二级加载器（`boot/boot.asm`），以 ATA LBA 读取（AH=42h / DAP）加载内核
- 内核加载至 `0x10000`，硬性上限 256 KB

**内核**
- 16 位实模式 → 32 位保护模式 → **x64 长模式**，全部在 `kernel/entry.asm` 中完成
- 在 32 位模式下构建恒等页表（PML4 + PDPT + 2 MB 大页）
- 完整映射 1 GB 不可缓存 MMIO 窗口，保证帧缓冲可访问
- 16 字节 x64 门描述符的 IDT、中断桩、符合 SysV 约定的寄存器保存/恢复
- APIC + IO-APIC，具备中断优先级（TPR）与向量分级
- 软中断模式：上半部只负责缓存输入，下半部在空闲循环中解码
- ATA PIO 驱动、exFAT 文件系统（长文件名、分配位图、FAT 链）、串口控制台（UTF-8）

**图形**
- Bochs VBE 线性帧缓冲，LFB 地址通过 PCI 配置空间探测获得
- 双缓冲——所有绘制先写入后台缓冲再翻转，因此无闪烁
- 文本控制台在帧缓冲上以 640x480 软渲染，中文不受 VGA 256 槽限制

**字体**
- 16x16 灰度字模，以原生 16 px、GDI `CLEARTYPE_QUALITY` 渲染，填满整个单元
- 每像素 4 bit、16 级灰度，绘制时与背景做 alpha 混合
- 约 21,400 个字模（ASCII、拉丁字母、符号与中文）共用自适应基线，
  因此中英文可以整齐排列在同一行
- 由 `tools/mkfont.py` 生成 `build/font.gtd`（2.75 MB）与 `cpm.gtd`

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

- 基于 exFAT 的 `open/read/write/close/seek` fd 层（fd 0/1/2 为控制台），
  目录句柄经 `SYS_OPENDIR`/`SYS_READDIR` 遍历
- `RUN <文件.gtp>` 将平面二进制载入暂存区并跳转执行；程序经 `SYS_EXIT` 返回 shell
- `.gtl` 共享库：加载器把镜像映射到 `LIB_BASE`（0x500000），
  由 `SYS_LIBLOAD`/`SYS_LIBSYM` 按名解析导出符号；每个程序运行前重置库命名空间
- `.gtd` 数据文件通过同一 fd 层读取（示例程序读取 `DATA.GTD`）
- `kernel/include/` 下的公开头文件（`gtos.h`、`unistd.h`、`stat.h`、`dirent.h`、`gtlib.h`、
  `time.h`、`stdint.h`、`ctype.h`、`stdio.h`、`string.h`、`stdlib.h`）
  被种子写入磁盘的 `INCLUDE` 目录

**网络**
- RTL8139 PCI 网卡驱动，轮询式收/发环（不依赖网卡中断）
- Ethernet II + ARP 缓存；IPv4 头部校验；ICMP echo（`PING`）
- UDP 与 DHCP 客户端（`NET` / `DHCP`）；显示链路、MAC、IP、掩码、网关与 DNS
- TCP：主动/被动连接、MSS 选项、序号/确认、接收窗口、超时重传、按序缓存，
  以及 FIN 四次挥手（`FIN_WAIT_1/2`、`CLOSE_WAIT`、`LAST_ACK`、`CLOSING`）
- 系统调用 21–28，以及 TC 内置 `ip4`、`tcp_connect`、`tcp_send`、`tcp_recv`、
  `tcp_close`；Shell 客户端 `TCP <ip> <端口> [文本]`
- QEMU 用户网络默认地址 `10.0.2.15/24`，网关 `10.0.2.2`

**内置 C 编译器 (TC) 与 SDK**
- `TC <源.c> [-o 输出.gtp]` 直接把 C 子集编译为 x86-64 机器码，生成可由 `RUN`
  执行的 `.gtp` 程序，全程不依赖外部工具链
- 支持：`int`/`char`/指针变量、常量初始化的全局变量、最多 6 个参数的函数与递归、
  `if/else`、`while`、`for`、`break`/`continue`/`return`、算术/位/比较运算、
  `&& || !`、字符串字面量与注释
- 内置函数（`\SDK\TC.H`）：`putc`、`print`、`puts`、`print_int`、`getc`、`exit`、
  `sleep`、`uptime`、`open`、`read`、`write`、`close`、`seek`、`stat`、`statx`、
  `reboot`、`shutdown`；`\SDK\NET.H` 另含 `ip4`、`tcp_connect`、`tcp_send`、
  `tcp_recv`、`tcp_close`
- `\SDK\` 提供明文只读 SDK：`TC.H`、`HELLO.C`、`FIB.C`、`README.TXT`；
  配合 `PUT` 与 `APPEND` 可在系统内逐行编写源码
- 程序布局：代码 64KB 位于 `0x600000`，数据/全局区位于 `0x610000`

**exFAT 厂商扩展**
- 每个文件/目录的目录项集附带 3 个厂商扩展项（类型 `0xE0`）：
  加密元数据（标志、算法、明文长度、CRC32）、ACL（所有者与权限位）、版本与 CRC32
- `SYS_STATX` 把它们暴露给程序（`flags`/`version`/`crc`/`owner`/`mode`）；
  非管理员写入时内核按所有者校验

**安全**
- `\GT-DOS\` 下所有文件以 AES-128-CBC（PKCS#7 填充，T 表高速路径）密文存储；
  内核内部读取时透明解密
- 镜像格式化与加密完全由独立 C 工具 `tools/gtmkfs.c` 完成（与内核链接同一份
  `kernel/crypto.c`）；构建脚本只生成种子清单并调用一次该工具
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
| Python 3.8+ | 用于运行 `build.py` 与 `tools/mkfont.py` |
| NASM | `nasm.exe` 需在 `PATH`，或位于 `C:\Program Files\NASM` |
| LLVM / clang | `clang.exe`、`ld.lld.exe`、`llvm-objcopy.exe` 需在 `D:\LLVM\bin` 或 `C:\Program Files\LLVM\bin` |
| QEMU | `qemu-system-x86_64.exe` 需在 `qemu/` 目录或 `PATH` 中 |

`build.py` 会在 `CANDIDATES` 表中列出的位置查找工具链；若你的工具链在别处，请修改该表。

### 编译与运行

```sh
python build.py            # 编译, 生成 build/gt-dos-hd.img
python build.py run        # 用 QEMU 启动镜像
python build.py run -nogui # 仅串口输出, 不开图形窗口
python build.py run -debug # 启动并开启 CPU/中断跟踪日志
python build.py all        # 先编译再运行
python build.py clean      # 清理 build 目录
```

首次编译会一并生成字模：当 `build/font.gtd` 或 `build/cpm.gtd` 缺失时，
会自动调用 `tools/mkfont.py`，耗时明显长于后续编译。

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

**系统** — `HELP`/`?`、`VER`、`CLS`、`DATE`、`TIME`、`MEM`、`CPUID`、`UPTIME`、`BEEP`、
`COLOR`、`SERIAL [OFF]`

**用户** — `WHOAMI`、`USERS`、`LOGIN`、`LOGOUT`/`EXIT`、`PASSWD`、`USERADD`、`USERDEL`、
`HOSTNAME`、`CONFIG`、`OOBE`（`USERADD`、`USERDEL`、`HOSTNAME`、`SHUTDOWN`、`REBOOT`
需要管理员权限）

**文件与磁盘** — `CD`、`PWD`、`DRIVES`、`DIR`/`LS [/A]`、`TYPE`/`CAT`、`PUT`、`DEL`/`RM`、
`MKDIR`/`MD`、`RMDIR`、`TOUCH`、`ATTRIB`、`CP`/`COPY`、`MV`/`REN`、`HEAD`、`WC`、`TREE`、
`DF`、`DISK`、`MOUNT`、`APPEND`

**显示与硬件** — `APIC`、`IRQSTAT`

**网络** — `NET`、`DHCP`、`PING <ip> [次数]`、`TCP <ip> <端口> [文本]`

**程序** — `RUN <文件.gtp>`（需要管理员权限）、`TC <源.c> [-o 输出.gtp]`（C 子集编译器）

**电源** — `REBOOT`、`SHUTDOWN`/`POWEROFF`（需要管理员权限）

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
  exec.c             RUN 程序加载器 (把 .gtp 载入 PROG_BASE)
  gtlib.c            .gtl 共享库加载器与符号表
  tc.c               内置 C 子集编译器 (x86-64 代码生成)
  net.c              RTL8139 驱动 + ARP/IPv4/ICMP/UDP/DHCP
  tcp.c              TCP socket (握手/收发/重传/挥手)
  crypto.c           系统文件加密 (AES-128-CBC, T 表) + CRC32
  timer.c keyboard.c mouse.c serial.c    设备驱动
  softirq.c          输入下半部的延迟处理
  ata.c fat.c cfg.c user.c               存储, exFAT 驱动(含 fd 层), 配置, 账户
  gfx.c vga.c tui.c console.c ui.c       图形层, 文本控制台, 启动画面
  shell.c            命令解释器
  i18n.c power.c oobe.c lib.c            文案, 复位/关机, 首次启动向导, libc 桩
tools/
  mkfont.py          字模生成器 (GDI -> 4bit 灰度 font.gtd + cpm.gtd)
  gtmkfs.c           独立 exFAT 格式化/填充 + AES 加密工具 (纯 C)
  sdk/               系统内 SDK: tc.h, net.h, hello.c, fib.c, net.c, readme.txt
  hello.asm          示例程序 (INT 0x21 SYS_WRITE/SYS_EXIT), 种子为 HELLO.GTP
  demo.asm           示例程序: .gtd 读取/seek、stat、.gtl 库调用 (DEMO.GTP)
  sample_lib.asm     示例共享库, 导出 add/mul (SAMPLE.GTL)
  test_v03.py        端到端测试: OOBE, 登录, 文件命令
  test_v04.py        回归测试: 单盘 C:, GT-DOS 目录, 属性, 系统保护
  test_syscall.py    INT 0x21 门与 RUN 程序执行测试
  test_net.py        RTL8139/ARP/ICMP/DHCP/TCP 测试（宿主回显服务器）
build.py             构建脚本与镜像组装
```

测试脚本通过串口 socket 与 QEMU monitor socket 驱动 QEMU 实例，
因此它们校验的是真实启动输出，而非模拟数据。

## 说明与限制

- 内核目前为 314 扇区（160,364 字节），加载区间为 `0x10000` 到 `0x80000`。
  若要继续增大，需要调整加载地址。
- 大块零初始化缓冲（`font_buf`、码点表、后台缓冲）被放置在 1 MB 以上、
  标记为 `NOLOAD` 的 `.highbss` 段。若放在普通 `.bss` 中，会与 `0xA0000`
  处的 VGA 显存孔径重叠并破坏帧缓冲。
- TC 仅实现 C 子集：不支持数组、结构体、联合、浮点、`switch`、`?:`、`++/--`
  与可变参数函数；程序代码上限 64KB，全局/字符串区上限 16KB。
- Shell 文件命令（`TYPE`/`CP`/`MV`/`HEAD`/`WC`/`APPEND`）处理不超过 64KB 的文件。
- 网络栈仅支持 IPv4，不做 IP 分片重组；TCP 未实现拥塞控制、SACK 与窗口缩放，
  `TCP_ACCEPT` 一次只处理一个连接。
- 构建过程需要 Windows；但内核本身是完全独立的，不依赖宿主平台。

## 许可证

本项目基于 [MIT 许可证](LICENSE) 发布。Copyright (c) 2026 GotHCL458。

## 相关链接

- 仓库地址：<https://github.com/GotHCL458/gt-dos>
- 发行版（磁盘镜像）：<https://github.com/GotHCL458/gt-dos/releases>