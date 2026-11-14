# GT-DOS v0.9 实施计划

## 背景 (Context)
用户要求六项改动，并对模糊项选择了最大范围：
1. 加入更多系统 API + 头文件 → 实现真正的系统调用门
2. 修复内存泄漏 → 全面审计缓冲（内核无堆，泄漏实为 FAT 簇 + 栈/静态缓冲溢出）
3. 强化进程安全 → 输入校验 + 格式串 + 权限 + 栈保护 + 边界断言 + 程序执行模型
4. 去掉 OOBE 主题选择步骤
5. 去掉分辨率(RES)命令
6. （由 1/3 派生）INT 0x21 系统调用门 + `RUN` 程序执行 + 按用户主目录的文件写权限

目标产出 v0.9：一个带 INT 0x21 API 面、可加载运行外部 `.gtp` 程序、具备家目录级文件权限、修复了簇泄漏与缓冲溢出、并移除了主题/分辨率交互的 x64 教学内核。

## 已定决策
- 系统调用门 = **INT 0x21**（DOS 语义）。因 0x21 当前是 PIC 模式键盘 IRQ，需把 **PIC 重映射到 0x30/0x38**（向量 48–63）腾出 0x21。这是最高风险步骤，单独成提交，test_v04 的键盘+APIC 往返作为回归网。
- 文件权限 = **按用户主目录隔离**：`useradd` 时创建 `\USERS\<名>\`；登录后非管理员可写范围限定在自己家目录，管理员可写全盘（`\GT-DOS\` 系统保护不变）；读(DIR/TYPE/CD)对所有登录用户开放。
- 主题步骤移除后保留默认主题（cfg.c），`COLOR` 命令仍可改主题。
- RES 命令移除后，连带删除失效的 `gfx_set_mode/gfx_modes[]/gfx_mode_count/gfx_mode_size/vga_relayout`，文本网格固定 640x480 推导。

## 分阶段实施

### P1 — 移除 OOBE 主题步骤 + RES 命令 + 死代码（低风险，先做）
- `kernel/oobe.c`：删步骤 4 主题块（约 131–146）；标签 `[x/4]`→`[x/3]`（约 53/64/79）；完成流程（oobe_done/cfg_save/"Enjoy GT-DOS"）保留。
- `kernel/i18n.c`：删 `oobe.theme*` 键（约 50–56）；删 help.text 的 RES 行（EN+ZH）。
- `kernel/shell.c`：删 `cmd_res()`（约 927–961）与 `res` 分派分支（约 1071–1072）。
- `kernel/gfx.c`：删 `gfx_modes[]`/`gfx_mode_count`/`gfx_mode_size`/`gfx_set_mode`；**保留** `mode_apply`/`gfx_init`/`gfx_active`/`gfx_w`/`gfx_h`。
- `kernel/vga.c`：删 `vga_relayout()`（约 465–479）。
- `kernel/gt.h`：删 `struct gfx_mode`、`gfx_set_mode`、`gfx_modes` extern、`gfx_mode_count/size`、`vga_relayout` 声明。
- `tools/test_v03.py`：删主题应答（约 144–145）。`tools/test_v04.py`：删 `expect("Choose")+cmd("1")`（约 89–90）。**P1 与测试改动同提交**，否则两套回归失败。
- 验证：build + test_v03 + test_v04；手测首启 `[1/3]..[3/3]`、`RES`→Bad command、`COLOR 2` 仍生效。

### P2 — 缓冲全面审计 + FAT 簇泄漏修复
确认/修复点（均对照现有源码）：
- `fat.c dir_write_entry`（约 751–765）：第二次 `find_free_slot` 失败时，`free_chain(nclus)` 再返回 false（当前泄漏新分配簇）。
- `fat.c fat_delete`/`fat_rmdir`（约 849–973）与 `fat_create` 覆盖路径：改为**先标 0xE5 写回目录项、再 `free_chain`**，避免读失败留下指向已释放簇的活动目录项。
- `shell.c path_normalize`（约 56–112）：emit 循环加 `if (k >= PATHMAX-1) return false;`（用户可触发的栈溢出）。
- `shell.c scroll_console`（约 17–43）、`tui.c tui_splash` 进度条（约 137–166）、`shell.c cmd_df`（约 518–543）：所有手工 append 加 `k < cap` 钳制（抽 `s_putc` 小helper）。
- 审计通过、无需改（写入提交说明）：keyboard.c/mouse.c 环形缓冲、vga.c hist/status、ui.c hist、console.c、user.c/cfg.c/ata.c 各缓冲均有边界。
- 验证：build + 两套回归；手测 `CD \`+16×15 段路径→Invalid path 不崩；put/del/df 循环空闲簇计数稳定。

### P3 — 安全加固：输入/格式串/权限 + 家目录文件写权限
- 输入校验：`cmd_head -n` 的 `atoi_dec` 钳制 [1,1000]；其余 tokenize/resolve_path/cmd_color/cmd_attrib 已有边界，文档化。
- 格式串：给 `gt.h` 的 `kprintf` 加 `__attribute__((format(printf,1,2)))`、`kvprintf` 加 `(printf,1,0)`（审计无现存违规，编译应干净，锁定纪律）。
- 权限补齐：`MOUNT`、`DIV0` 加 `need_admin()`。
- **家目录文件权限（新功能）**：
  - `user.c`：`useradd` 时 `mkdir \USERS\<名>`；新增 `user_home_dir(name, out)` 返回 `\USERS\<名>\`（admin 返回根 `\`）。当前登录用户家目录在 `user_current()` 基础上缓存。
  - `fat.c`/`shell.c`：新增 `write_allowed(path)`：admin→仅拒 `\GT-DOS\`（沿用 `sys_write_denied`）；非管理员→仅允许路径前缀在自己家目录内，否则拒。把 `PUT/DEL/MKDIR/RMDIR/CP/MV/ATTRIB/TOUCH` 的写检查从 `sys_write_denied` 换/叠加 `write_allowed`。
  - 登录后非管理员默认 `cd` 到家目录（shell 会话初始路径）。
  - 读命令（DIR/TYPE/CD/PWD/WHOAMI/USERS）保持开放。
- 验证：build + 两套回归（回归多用 admin，家目录限制不破坏）；手测非管理员在家目录外 `put`→Permission denied、在家目录内成功；admin 全盘可写但 `\GT-DOS\` 仍受保护。

### P4 — INT 0x21 系统调用门 + fd 层 + 头文件 + gtos.h（最高风险，单独提交）
- 4a PIC 重映射：`idt.c pic_init` ICW2 `0x20/0x28`→`0x30/0x38`；`isr.asm` `IRQ n,32+n`→`48+n`；`idt.c isr_handler` PIC 范围 `32..48`→`48..64`、APIC 吸收分支同步、`idt_init` 循环 `32+i`→`48+i`；更新注释。
- 4b 门+桩：`isr.asm` 新增 `sysint`（push 0; push 33; jmp isr_common）；`idt.c idt_init` 加 `idt_set_gate(0x21,(u64)sysint,0x18,0x8E)`（选择子 0x18、标志 0x8E，与其它门一致）；`isr_handler` 加 `if (r->int_no==0x21){ syscall_handle(r); return; }`；`gt.h` 加 `SYSCALL_VECTOR`、`SYS_*` 号、`syscall_handle` 声明。
- 4c `kernel/syscall.c`（新）：按 `r->rax` 分派，参数取 SysV 寄存器(rdi/rsi/rdx/r10/r8/r9)，结果写回 `r->rax`。号：EXIT0/PUTC1/WRITE3/READ4/OPEN5/CLOSE6/UPTIME7/REBOOT8/SHUTDOWN9/GETTIME10/PUTS11；reboot/shutdown 以 `user_is_admin()` 把关；指针参数做非空校验；未知号返回 -1。
- 4d `kernel/fat.c` 末尾 fd 层（复用现有 fat_*）：`FD_MAX 8`，0/1/2 为控制台；`struct fd_entry` 含 first/size/pos/游标/path/写缓冲；写缓冲(8×4KB)放 `.highbss`；`fat_fd_open/read/write/close`（close 时写脏→`fat_create` 落盘，保留 `\GT-DOS\` 保护）。`gt.h` 声明。
- 4e 头文件（`kernel/include/`，build.py 自动 glob 到 INCLUDE，无需改 build.py）：新增 `stdint.h`/`ctype.h`(static inline)/`time.h`(`struct gt_time`+gt_gettime/gt_uptime_ms)/`unistd.h`(fd/flag 常量 + `static inline` 的 `int $0x21` 包装)；重写 `gtos.h` 对齐真实 ABI + 系统调用表 + 诚实标注“INT 0x21 是 API 入口，程序运行于 ring0，非权限边界”。
- 4f `build.py`：`C_SOURCES` 加 `"syscall.c"`。
- 验证：build（盯内核体积）；test_v03/test_v04（PIC 重映射被键盘输入+APIC 往返+双控制器文件 IO 充分覆盖）。

### P5 — 程序执行模型 `RUN <file.gtp>` + 示例程序
- `gt.h`：`PROG_BASE 0x600000`、`PROG_MAX 0x100000`、`PROG_STACK_TOP 0x7FF000`（空闲区 0x423000–0x7FFFFF 已核实）。
- `kernel.ld`：导出 `__highbss_end`；`exec_init()` 运行时校验 `PROG_BASE>__highbss_end` 且 `PROG_BASE+PROG_MAX<=0x800000`，否则 panic。
- `kernel/exec.c`+`kernel/exec.asm`（新）：`exec_run(abs)` 查目录项→拒目录/空/超限→`fat_read_file` 载入 PROG_BASE→`exec_active` 防重入→`exec_enter(entry,stack_top,ctx)`（asm 保存 rsp/rbp/rip 后跳程序，`.resume` 返回）；`exec_longjmp(ctx)` 供 SYS_EXIT 回跳。
- `syscall.c` SYS_EXIT：exec_active 时存码、清标志、`exec_longjmp`；否则 rax=-1。
- `shell.c`：`cmd_run`（`need_admin()`+`resolve_path`+`exec_run`，打印退出码）+ `run` 分派 + i18n help 的 `RUN <file.gtp>` 行（EN+ZH）。
- `build.py`：`C_SOURCES` 加 `"exec.c"`、`ASM_SOURCES` 加 `"exec.asm"`；新增 `build_demo_prog()` 汇编 `tools/hello.asm`→`build/hello.gtp` 并种子进 FAT `HELLO.GTP`。
- `tools/hello.asm`（新）：ORG 0 平面二进制，`int $0x21` 调 SYS_WRITE(fd=1,"Hello from INT 0x21\n") 后 SYS_EXIT(0)。
- 验证：新增 `tools/test_syscall.py`（仿 test_v04 的 VM 类）：`RUN HELLO.GTP`→见 Hello+退出码0+提示符恢复；`RUN NOPE.GTP`→File not found；超限拒绝；RUN 后键盘仍正常。

### P6 — 栈保护 + 边界断言（最后，复验全局）
- 首选编译器 SSP：`entry.asm` 加 `set_fs_base(rdi)`（`wrmsr` MSR 0xC0000102，仿 entry.asm:119–123 的 EFER 写法）；`kmain.c` 早期 `cpu_area[8]`（offset 0x28 存 64 位随机 canary，取自 rand_next）后 `set_fs_base`；实现 `__stack_chk_fail(){ panic("stack smashing"); }`；`build.py` CFLAGS `-fstack-protector`→`-fstack-protector-strong`。
- 回退（若 FS/链接/QEMU 受阻）：`gt.h` 手动 canary 宏，套用到 ui_read_line/execute 的 tokenize 调用/path_normalize/resolve_path/fat_read_dir_file/dir_write_entry。
- 边界断言：`gt.h` 加 `GT_ASSERT(cond)`（失败 `__builtin_trap()`→现有 isr6 打印并 halt）与 `GT_ASSERT_BOUNDED(idx,cap)`；在 sys_path/user_path out[32]、cmd_df p[40]、tui_frame line[84]、fd 表索引等处插入。
- 验证：全量 build（体积仍远低于 256KB 上限）+ test_v03/test_v04/test_syscall。

## 关键文件
- `kernel/shell.c`（P1 RES、P2/P3 输入+权限、P3 家目录、P5 RUN）
- `kernel/fat.c`（P2 泄漏、P4 fd 层）
- `kernel/idt.c` + `kernel/isr.asm`（P4 PIC 重映射 + INT 0x21 门）
- `kernel/gt.h`（全部新声明/常量：SYS_*、fd API、PROG_*、GT_ASSERT）
- `kernel/user.c`（P3 家目录创建/查询）
- `kernel/oobe.c`/`kernel/i18n.c`/`kernel/gfx.c`/`kernel/vga.c`（P1 移除）
- `kernel/syscall.c`/`exec.c`/`exec.asm`（P4/P5 新文件）
- `kernel/include/gtos.h` + 新头文件（P4）
- `build.py`（C_SOURCES/ASM_SOURCES 追加、hello.gtp 种子）
- `tools/test_v03.py`/`test_v04.py`（P1 流程改动）、`tools/test_syscall.py`/`hello.asm`（P5 新增）

## 验证（整体）
1. 每阶段：`python build.py` 通过，内核体积打印 < 256KB。
2. P1–P3：`test_v03.py` + `test_v04.py` 全绿（P1 需同步改测试）。
3. P4：两套回归全绿（覆盖 PIC 重映射后的键盘/定时器/APIC 往返/文件 IO）。
4. P5：`test_syscall.py` 全绿 + 手动 `RUN HELLO.GTP`。
5. P6：三套测试全绿。
6. 收尾：`python build.py run` 手测首启 3 步向导、登录、家目录写限制、`RUN`、`COLOR`、`APIC on/off`。

## 风险与约束
- **P4a PIC 重映射**动到键盘/定时器中断路径，风险最高 → 单独提交，test_v04 兜底；isr.asm 向量常量与 idt.c 范围分支必须同步改。
- INT 0x21 门用选择子 0x18/标志 0x8E（16 字节门，ring0）；DPL0 正确（程序运行于 ring0，GDT 无 ring3 段）。
- 新 `.c/.asm` 必须手动加进 build.py 的 C_SOURCES/ASM_SOURCES；头文件是 glob 自动。
- `.highbss` 增长纪律：fd 写缓冲与 exec 布局共用 0x423000–0x7FFFFF；exec_init 的 `__highbss_end` 校验让未来冲突在启动时大声失败。
- 256KB 内核上限：现 89KB，P4+P5+P6 估增 25–40KB，盯体积打印。
- 家目录权限改变非管理员会话初始路径与可写范围，需与现有 `\GT-DOS\` 保护协调。
- README 仍描述 RES/主题（本次不擅改文档，留作后续文档 pass）。
- 建议按 P1→P6 分 6 次提交，每次回归绿后再进下一步；版本号统一升到 0.9。
