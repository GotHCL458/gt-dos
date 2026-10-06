# GT-DOS

[English](README.md) | [简体中文](README.zh-CN.md)

GT-DOS 是一个从零实现的 **x64 教学操作系统**，使用 NASM 汇编与 C（clang/LLVM 编译）编写。
它通过自己的 MBR 与二级加载器，从单个 64 MB 硬盘镜像引导进入 64 位长模式，并提供文本控制台、
图形桌面、FAT16 文件系统、用户账户与中英双语 Shell。

![version](https://img.shields.io/badge/version-0.7-blue)

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
- ATA PIO 驱动、FAT16 文件系统、串口控制台（UTF-8）

**图形**
- Bochs VBE 线性帧缓冲，LFB 地址通过 PCI 配置空间探测获得
- 双缓冲——所有绘制先写入后台缓冲再翻转，因此无闪烁
- 运行时切换分辨率：640x480 / 800x600 / 1024x768 / 1280x1024
- GUI 桌面：窗口、拖拽、标题栏、图标与任务栏

**字体**
- 16x16 灰度中文字模，以原生 14 px、GDI `CLEARTYPE_QUALITY` 渲染
- 每像素 4 bit、16 级灰度，绘制时与背景做 alpha 混合
- 约 21,500 个字模（ASCII、拉丁字母、符号与中文）共用同一基线，
  因此中英文可以整齐排列在同一行
- 由 `tools/mkfont.py` 生成 `build/font.bin`（2.75 MB）与 `cpm.bin`

**系统**
- OOBE 首次启动向导（语言 → 主机名 → 管理员 → 主题）
- 带口令的用户账户；`admin` 拥有管理员权限
- i18n 层，提供完整的中英文文案

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

首次编译会一并生成字模：当 `build/font.bin` 或 `build/cpm.bin` 缺失时，
会自动调用 `tools/mkfont.py`，耗时明显长于后续编译。

直接启动已发布的镜像：

```sh
qemu-system-x86_64 -hda gt-dos-hd.img -boot c
```

首次启动会进入 OOBE 向导，依次询问语言、主机名、管理员名与主题。
输入为空会重新询问；`Ctrl-C` 则以默认值跳过当前项。

## 磁盘布局

镜像为单个 64 MB 原始硬盘，带 MBR 分区表：

| 区域 | 位置 | 内容 |
| --- | --- | --- |
| MBR | LBA 0 | `boot/mbr.asm`，分区表由 `build.py` 写入 |
| 分区 1（C:） | LBA 2048，120832 扇区 | FAT16 系统盘，含 `\GT-DOS\` 目录与 `INCLUDE` 头文件 |
| 分区 2（隐藏） | 盘尾 8192 扇区 | 启动分区，类型 `0x11`，带可引导标志：`boot.bin` + `kernel.bin` + `font.bin` + `cpm.bin` |

启动分区位于磁盘末尾并携带可引导标志；内核在启动时通过 ATA 从中读取 `font.bin` 与 `cpm.bin`。

## Shell 命令

在 GT-DOS 中输入 `HELP`（或 `?`）可查看完整列表。命令同时支持长名与短名。

**系统** — `HELP`/`?`、`VER`、`CLS`、`DATE`、`TIME`、`MEM`、`CPUID`、`UPTIME`、`BEEP`、
`COLOR`、`SERIAL [OFF]`

**用户** — `WHOAMI`、`USERS`、`LOGIN`、`LOGOUT`/`EXIT`、`PASSWD`、`USERADD`、`USERDEL`、
`HOSTNAME`、`CONFIG`、`OOBE`（`USERADD`、`USERDEL`、`HOSTNAME`、`SHUTDOWN`、`REBOOT`
需要管理员权限）

**文件与磁盘** — `CD`、`PWD`、`DRIVES`、`DIR`/`LS [/A]`、`TYPE`/`CAT`、`PUT`、`DEL`/`RM`、
`MKDIR`/`MD`、`RMDIR`、`TOUCH`、`ATTRIB`、`CP`/`COPY`、`MV`/`REN`、`HEAD`、`WC`、`TREE`、
`DF`、`DISK`、`MOUNT`

**图形与硬件** — `GUI`、`RES <W>x<H>` / `RES LIST`、`APIC`、`IRQSTAT`

**电源** — `REBOOT`、`SHUTDOWN`/`POWEROFF`（需要管理员权限）

## 项目结构

```
boot/
  mbr.asm            MBR, 负责加载二级加载器
  boot.asm           二级加载器, 加载内核并传递元数据
kernel/
  entry.asm          实模式 -> 保护模式 -> 长模式, 页表构建
  isr.asm            中断桩与公共寄存器帧
  kernel.ld          链接脚本; 大缓冲位于 1MB 以上的 .highbss
  gt.h               内核全局定义
  include/           系统头文件 (stdio.h, stdlib.h, string.h, gtos.h)
  kmain.c            内核入口
  idt.c apic.c isr   中断: IDT, APIC/IO-APIC, 分发
  timer.c keyboard.c mouse.c serial.c    设备驱动
  softirq.c          输入下半部的延迟处理
  ata.c fat.c cfg.c user.c               存储, 文件系统, 配置, 账户
  gfx.c vga.c tui.c console.c ui.c       图形层, 文本控制台, 启动画面
  gui.c              图形桌面
  shell.c            命令解释器
  i18n.c power.c oobe.c lib.c            文案, 复位/关机, 首次启动向导
tools/
  mkfont.py          字模生成器 (GDI -> 4bit 灰度 font.bin + cpm.bin)
  test_v03.py        端到端测试: OOBE, 登录, 文件命令
  test_v04.py        回归测试: 单盘 C:, GT-DOS 目录, 属性, 系统保护
  verify_gui.py      基于抓屏像素分析的 GUI 验证
build.py             构建脚本与镜像组装
```

测试脚本通过串口 socket 与 QEMU monitor socket 驱动 QEMU 实例，
因此它们校验的是真实启动输出，而非模拟数据。

## 说明与限制

- 内核目前为 198 扇区（100,992 字节），加载区间为 `0x10000` 到 `0x80000`。
  若要继续增大，需要调整加载地址。
- 大块零初始化缓冲（`font_buf`、码点表、后台缓冲）被放置在 1 MB 以上、
  标记为 `NOLOAD` 的 `.highbss` 段。若放在普通 `.bss` 中，会与 `0xA0000`
  处的 VGA 显存孔径重叠并破坏帧缓冲。
- 构建过程需要 Windows；但内核本身是完全独立的，不依赖宿主平台。

## 许可证

本项目基于 [MIT 许可证](LICENSE) 发布。Copyright (c) 2026 GotHCL458。

## 相关链接

- 仓库地址：<https://github.com/GotHCL458/gt-dos>
- 发行版（磁盘镜像）：<https://github.com/GotHCL458/gt-dos/releases>