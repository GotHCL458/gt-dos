# 规格：GUI 化登录/OOBE + 完整 PNG 渲染

## 1. 目标

1. **GUI 化 OOBE 与登录**：在图形模式可用时，首启向导与登录页改为全屏图形界面；
   仍保留串口/PS-2 键盘输入，且**串口输出文本与现有自动化测试完全兼容**。
2. **完整 PNG 渲染**：实现完整 PNG 解码（zlib inflate + 全部颜色类型/位深 + 5 种过滤 +
   Adam7 隔行 + tRNS/alpha），并接入图形层与 GUI 界面（OOBE/登录背景、桌面看图命令）。

## 2. 非目标

- 不改变文本模式（无 VBE）下的 OOBE/登录行为。
- 不引入 PNG 编码器；不重构桌面窗口系统。
- 不修改已有系统调用号/ABI。
- GUI 登录仅在 `gfx_active() && !gui_active()`（启动阶段/纯文本态）启用；在桌面中用
  `login`/`logout` 命令时沿用原终端内登录，避免与 `gui_pump` 桌面重绘冲突。

## 3. 设计决策（已确定）

- **串口兼容优先**：GUI 界面不改变任何 `L(key)` 文案与输出顺序；只把"输入回显"改为
  画在图形界面上，不再向串口回显逐字符（现有测试均依赖后续提示串，不依赖键入回显）。
- **驱动输入**：GUI 文本字段用 `ui_read_key()` 逐键读取，返回码与 `ui_read_line()` 一致：
  `0`=回车提交、`1`=Ctrl-C 放弃、`-1`=Ctrl-Alt-Del。
- **绘制隔离**：GUIS 屏幕期间 `vga_hold_display(true)`，结束时 `vga_hold_display(false)`。
- **内存**：PNG 解码流式进行，逐行渲染到 gfx 后台缓冲，仅保留两行原始像素 + 一行过滤行；
  压缩数据通过"IDAT 块串联读取器"边读边解压，避免整幅输出缓冲。

## 4. 新增/修改文件

### 4.1 新增 `kernel/gscreen.c`（GUI 认证界面）

依赖：`gt.h`、gfx 层、i18n、user、cfg、timer、serial（经 console）。

导出函数（声明加入 `gt.h`）：

```c
bool gs_available(void);                    /* gfx_active() && !gui_active() */
void gs_oobe_run(void);                     /* GUI 首启向导 */
const struct gt_user *gs_login_screen(void);/* GUI 登录 */
```

内部实现要点：

- `static void gs_backdrop(void)`
  - 先尝试 `png_draw_file_fit("WALLPAPER.PNG")`（根目录，若存在且解码成功）；
  - 否则绘制竖向渐变背景（复用蓝/紫配色，居中标题水印）。
- `static void gs_card(const char *title, const char *sub)`
  - `gfx_clear()` 背景 → 绘制居中面板（底板 + 双层描边 + 标题栏 + 副标题 + 页脚提示）。
  - 记录面板/字段矩形到静态全局，供字段刷新。
- `static void gs_lines(const char *const *lines, int n)`：把多行文案画在面板正文区。
- `static void gs_field_draw(const char *label, const char *buf, int len, int mask, int cur)`
  - 画标签 + 输入框（内部白底 + 光标）；mask!=0 时用 `*` 代替字符。
  - 只重绘字段矩形并 `gfx_flip_rect()`。
- `int gs_read_field(const char *prompt, char *buf, int max, int mask)`
  - `console_puts(prompt)`（串口+历史，屏幕因 hold 不绘制）；GUI 画标签并进入键盘循环。
  - 支持：ASCII 32..126 插入；退格 `'\b'/127`；`'\r'/'\n'` 提交；`3`=Ctrl-C；
    `27`=ESC 清空；`KEY_LEFT/RIGHT/HOME/END/DEL` 光标编辑（可选，最小实现至少支持退格/提交/取消）。
  - 每次按键后 `gs_field_draw()` 局部刷新。
- `static int gs_ask_line(const char *prompt, char *buf, int max)`：空输入重问，Ctrl-C 返回 -1（同旧 `ask_line`）。
- `static int gs_ask_choice(const char *prompt, int min, int max)`：读字段→数字校验→范围校验，
  错误时 `ui_err("  Please enter a number.\n")` / `ui_err("  Out of range (%d-%d).\n", ...)` 并重问（同旧逻辑）。

`gs_oobe_run()` 流程（与旧 `oobe_run` 文案/顺序/副作用完全一致）：

```
vga_hold_display(true); console_clear(); tui_status_refresh();
gs_card(L("oobe.title"), 欢迎语);
/* 步骤1 语言 */
ui_info("  [1/3] %s\n", L("oobe.lang"));
kprintf("%s\n", L("oobe.lang_en")); kprintf("%s\n", L("oobe.lang_zh"));
lc = gs_ask_choice(L("oobe.lang_q"), 1, 2);
i18n_set_lang(lc==2); gcfg.lang = ...; if (lc==2) ui_ok(L("oobe.lang_set"));
/* 步骤2 主机名 */
ui_info("  [2/3] %s\n", L("oobe.host")); kprintf("%s", L("oobe.host_desc"));
若 gs_ask_line(L("oobe.host_q"),...) == 0 且非空 → 写 gcfg.hostname + ui_ok(L("oobe.host_set"))
否则 ui_info(L("oobe.host_def"), gcfg.hostname)
/* 步骤3 管理员 */
ui_info("  [3/3] %s\n", L("oobe.admin"));
循环 gs_ask_line(L("oobe.admin_q"))：user_add/ user_exists → ui_ok(L("oobe.admin_created"),buf) 并记 last_user；
                           非法 → ui_err(L("oobe.admin_bad"), UNAME_MAX-1)
ui_info("%s", L("oobe.pass_q"));
p1 = gs_read_field(L("oobe.pass"), ..., '*'); 若 p1[0] 则 p2 = gs_read_field(L("oobe.pass2"), ..., '*');
一致则 user_setpass + ui_ok(L("oobe.pass_set"), admin_name)，否则 ui_err(L("oobe.pass_mismatch"))；user_save();
若磁盘不可用 → ui_err(L("oobe.skip"))
/* 完成 */
gcfg.oobe_done = true; cfg_save(); ui_ok("\n%s", L("oobe.done"));
timer_sleep_ms(800); vga_hold_display(false); vga_refresh();
```

`gs_login_screen()` 流程（与旧 `ui_login_screen` 文案/返回语义一致）：

```
vga_hold_display(true); gs_card(L("login.title"), L("login.hint"));
result = &default_user（需通过 user_current 兜底等价：返回默认 admin）
for tries in 0..4:
    console_puts(L("login.user")); name = gs_read_field(...);
    r==-1 -> power_reboot(); r==1 -> break;
    name 空 -> ui_err(L("login.empty")); continue;
    !user_exists -> ui_err(L("login.nouser")); continue;
    console_puts(L("login.pass")); pass = gs_read_field(..., '*');
    r==-1 -> power_reboot(); r==1 -> break;
    校验成功 -> ui_ok(L("login.welcome"), name, admin?" (admin)":""); result=&users[i]; break;
    失败 -> ui_err(L("login.wrong")); timer_sleep_ms(600);
vga_hold_display(false); vga_refresh();
return result;
```

> 注：为让 GUI 登录写回用户，需在 `gt.h` 暴露一个内部访问器，或让 `gs_login_screen()` 直接
> 使用 `user_get/user_count` 查找并返回指针（`user_get` 已导出）。取消时返回一个稳定默认用户：
> 若存在 "admin" 返回该指针，否则回退 `user_current()`。

### 4.2 修改 `kernel/oobe.c`

`oobe_run()` 开头：

```c
void oobe_run(void)
{
    if (gs_available()) { gs_oobe_run(); return; }
    ... 原文本实现保持不变 ...
}
```

### 4.3 修改 `kernel/user.c`

`ui_login_screen()` 开头：

```c
const struct gt_user *ui_login_screen(void)
{
    if (gs_available()) return gs_login_screen();
    ... 原文本实现保持不变 ...
}
```

### 4.4 修改 `kernel/gt.h`

- 增加 `gs_available/gs_oobe_run/gs_login_screen` 声明（放在 GUI 区）。
- 增加 PNG 声明与信息结构：

```c
struct png_info { int w, h; u8 depth, ctype, interlace; };
int  png_info(const u8 *data, u32 len, struct png_info *out); /* 0=ok,<0=错误 */
bool png_draw(const u8 *data, u32 len, int dx, int dy);        /* 1:1 alpha 混合到后台缓冲 */
bool png_draw_file(const char *path, int dx, int dy);          /* 从 FAT 读文件再 png_draw */
bool png_draw_file_fit(const char *path);                      /* 居中缩放铺满屏幕 */
```

- 增加 `void gfx_pixel_a(int x, int y, u32 rgb, u8 a);`（gfx 后台缓冲按 alpha 混合写点）。

### 4.5 修改 `kernel/gfx.c`

新增：

```c
void gfx_pixel_a(int x, int y, u32 rgb, u8 a)
{
    if ((unsigned)x >= (unsigned)gw || (unsigned)y >= (unsigned)gh) return;
    u32 *p = &back[y * gw + x];
    if (a == 0) return;
    if (a == 255) { *p = rgb; return; }
    u32 bg = *p, ia = 255 - a;
    u32 r = (((bg>>16)&255)*ia + ((rgb>>16)&255)*a)/255;
    u32 g = (((bg>>8)&255)*ia + ((rgb>>8)&255)*a)/255;
    u32 b = ((bg&255)*ia + (rgb&255)*a)/255;
    *p = (r<<16)|(g<<8)|b;
}
```

### 4.6 新增 `kernel/png.c`（完整解码 + 渲染）

内存（`.highbss`）：

```c
#define PNG_FILE_MAX (512*1024)
#define PNG_ROW_MAX  2048
static u8 png_file[PNG_FILE_MAX] __attribute__((section(".highbss")));
static u8 png_prev[PNG_ROW_MAX*8]  __attribute__((section(".highbss")));  /* 上一行原始字节 */
static u8 png_cur [PNG_ROW_MAX*8]  __attribute__((section(".highbss")));  /* 当前行原始字节 */
static u8 png_line[PNG_ROW_MAX*8]  __attribute__((section(".highbss")));  /* inflate 输出(含过滤字节) */
```

组件：

1. **块解析**：遍历 PNG 签名 `89 50 4E 47 0D 0A 1A 0A` 后的大端 length/type/data/crc。
   - `IHDR` → 宽/高/位深/颜色类型/压缩/过滤/隔行。
   - `PLTE` → 调色板（最多 256 项 RGB）。
   - `tRNS` → 透明色/alpha 表。
   - `IDAT` → 记录首个与结束范围（连续块按顺序流式读取）。
   - `IEND` → 结束。忽略其它块；CRC 不强制校验（可计算但不阻断）。
2. **IDAT 流读取器**：状态保存当前 IDAT 块偏移；`z_byte()` 返回下一压缩字节，块用尽自动跳到下一个
   连续 `IDAT`；无更多数据返回 -1。
3. **inflate（puff 风格）**：
   - 位缓冲 `bitbuf/bitcnt`，`getbits(n)`。
   - 块类型：00 存储块（对齐后直接拷贝）、01 固定 Huffman、10 动态 Huffman（读 HLIT/HDIST/HCLEN
     与码长序列，含 16/17/18 重复码）、11 错误。
   - 规范 Huffman 构造：`count[16]` + `symbol[288]`，解码用逐位 `code/first/index` 算法。
   - 长度/距离表为标准 DEFLATE 常量；`z_inflate(out, n)` 支持"输出够 n 字节即返回"，可跨行恢复。
4. **过滤重建**：对每个字节应用 filter 0=none、1=sub、2=up、3=average、4=paeth（bpp = 每像素字节数）。
5. **像素取值**（支持位深 1/2/4/8/16）：
   - 灰度 0：按位深展开到 8/16 位；16 位取高字节。
   - 真彩 2：R,G,B（16 位取高字节）。
   - 调色 3：索引 → PLTE，索引 < tRNS 长度时取该项 alpha，否则 255。
   - 灰+alpha 4：Y,A。
   - 真彩+alpha 6：R,G,B,A。
   - 非调色 tRNS：灰度/RGB 匹配透明色时 alpha=0。
6. **Adam7 隔行**：`interlace==1` 时按 7 个 pass 的 `x0,y0,dx,dy` 依次解码；每个 pass 独立维护 prev 行；
   像素写到对应屏幕坐标。`interlace==0` 顺序逐行。
7. **渲染**：`png_draw(data,len,dx,dy)` 直接对每个解码像素调用 `gfx_pixel_a(dx+x, dy+y, rgb, a)`；
   越界自动裁剪（`gfx_pixel_a` 内已判界）。`png_draw_file()` 先 `fat_find` 取大小 →
   `fat_read_file` 到 `png_file` → `png_draw`。`png_draw_file_fit()` 计算
   `s=min(gfx_w()/w, gfx_h()/h)`（至少 1），最近邻放大：先解码到临时按源尺寸逐行? ——
   若 `w<=PNG_ROW_MAX` 且 `w*h` 可接受，则用 `png_draw` 1:1 居中；否则用整数倍最近邻：
   逐目标行映射源行、逐目标列映射源列，从"上一行/当前行原始字节"取值（只支持放大，缩小走 1:1 居中裁剪）。

> 为控制复杂度，`png_draw_file_fit` 的缩放实现为**整数倍最近邻放大 + 居中**；若图像大于屏幕则
> 1:1 居中裁剪。这满足"完整解码/渲染"，缩放非 PNG 规范范畴。

### 4.7 修改 `kernel/shell.c`

新增两个命令：

- `SHOW <file.png>`：若 `gfx_active()`，进入看图模式：若桌面激活用 `gui_canvas_begin()`
  （失败则 `vga_hold_display(true)`），`gfx_clear(0x101820)` → `png_draw_file_fit(file)` →
  `gfx_flip()` → 提示 "Press any key..." → `ui_read_key()` → 退出并恢复
  （`gui_canvas_end()` 或 `vga_hold_display(false)`）。若无图形模式 → 报错。
- `PNGINFO <file.png>`：读取文件头，打印 `w x h, depth=N, color=TYPE, interlace=N`。

在 HELP 文本（中英）中补一行说明。

### 4.8 修改 `build.py`

- `C_SOURCES` 增加 `"gscreen.c"` 与 `"png.c"`。
- 新增 `build_png_assets()`：用纯 Python（`zlib` + `struct` + 手写 chunk/CRC）生成测试 PNG，
  覆盖：
  - `SAMPLE.PNG`：RGBA8（颜色类型 6），含 alpha 渐变；
  - `RGB8.PNG`：RGB8（类型 2），逐行使用 filter 0..4 混合；
  - `PAL8.PNG`：调色板 8 位（类型 3）+ tRNS 透明项；
  - `GRAY.PNG`：灰度 8 位（类型 0）；
  - `GA.PNG`：灰度+alpha（类型 4）；
  - `RGB16.PNG`：RGB 16 位（类型 2, depth 16）；
  - `PAL1.PNG`：调色板 1 位（类型 3, depth 1）；
  - `ADAM7.PNG`：RGBA8 + Adam7 隔行；
  - `WALLPAPER.PNG`：1024x768 渐变壁纸（RGBA8，用作 OOBE/登录背景）。
  这些文件加入 `assets`（根目录明文），随镜像种子写入。
- 生成 PNG 的编码器放在 `build.py` 内部函数，支持指定 color type/depth/过滤/隔行。

### 4.9 测试

- 新增 `tools/test_png.py`：
  - 启动 → OOBE（`Language`/`1`/`SBOX`/`admin`/``）→ 登录（`admin`/``）→ 桌面；
  - 在该测试中默认 GUI 登录/OOBE 会走图形路径，验证串口仍能完成握手（与 test_gui 相同流程）；
  - `PNGINFO SAMPLE.PNG` → 断言输出含尺寸/类型；`PNGINFO PAL8.PNG`、`PNGINFO ADAM7.PNG`；
  - `SHOW SAMPLE.PNG` → 截屏，断言中心像素为已知颜色；按键退出；
  - `SHOW ADAM7.PNG` → 截屏，断言隔行图像像素正确；
  - 结果输出 `RESULT: ALL PASS`。
- 现有脚本（`test_v03/v04/gui/gui_lib/kbd/syscall/tasks/net/perf/protect`）**无需改动**，
  仅在回归时确认全部通过。

### 4.10 README

更新 `README.md` 与 `README.zh-CN.md`：
- 新增"GUI 登录/OOBE"与"PNG 渲染（SHOW/PNGINFO）"说明；
- 更新测试列表（新增 `test_png.py`）；
- 更新内核尺寸（构建后实测扇区/字节）。

## 5. 串口兼容矩阵（必须逐条保持）

| 阶段 | 必须出现的串口文本（来源 key） |
|------|------------------------------|
| OOBE 语言 | `Language` / `English` / `Chinese` |
| OOBE 主机名 | `Host name`（`oobe.host_q`）/ 设置后 `Host name set to 'X'.` |
| OOBE 管理员 | `user name`（`oobe.admin_q`）/ `Admin user 'X' created...` |
| OOBE 口令 | `Password`（`oobe.pass`/`oobe.pass2`）/ `Password set for 'X'.` |
| OOBE 完成 | `Setup complete. Enjoy GT-DOS!` |
| 登录 | `GT-DOS Login` / `User:` / `Password:` / `User name cannot be empty.` / `No such user.` / `Wrong password.` / `Welcome, X.` |

## 6. 验收标准

- `python build.py` 通过，镜像内核 ≤ 640 扇区。
- `tools/test_v03.py`、`test_v04.py`、`test_gui.py`、`test_gui_lib.py`、`test_kbd.py`、
  `test_syscall.py`、`test_tasks.py`、`test_net.py`、`test_perf.py`、`test_protect.py`
  全部 `ALL PASS`。
- 新增 `tools/test_png.py` 全部 PASS。
- GUI 登录/OOBE 截图可见图形界面（非纯文本）。
