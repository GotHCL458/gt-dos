# 规格：桌面完善（无闪烁切换 / 可用应用 / 交互 / 共享库图标）

> 里程碑：在已完成 GUI 登录/OOBE + PNG 渲染的基础上，完善图形桌面交互与共享库图标支持。
> 前序规格与任务已完成（见 git 历史）。

## 1. 目标（对应用户请求）

1. **消除 GUI→CLI 闪烁**：点击 TEXT / `TEXTMODE` 时只发生一次整屏提交，不再先重绘视口再整屏、
   不再把桌面后台缓冲局部恢复后又被覆盖。
2. **桌面软件真正可用**：桌面图标 TERM / FILES / SYS 可双击运行；FILES 列出 C: 根目录，
   SYS 显示系统信息；TERM 聚焦/还原终端窗口。
3. **OOBE 支持默认选项**：语言/主机名/管理员/口令均可直接回车采用默认值；保留全部串口提示串。
4. **拖动**：桌面图标可拖动；拖入终端客户区（drop）触发对应应用并输出串口事件。
5. **右键菜单**：桌面空白处与图标上右键弹出上下文菜单（Open / Properties / 快捷启动）。
6. **属性对话框**：桌面与图标属性（名称、类型、大小、位置、分辨率、运行时间、主题、磁盘）。
7. **共享库嵌入图标**：`.gtl` 末尾可携带 `GTI1` 图标块；Rust 解析器/装载器/渲染器支持，
   FILES 窗口中显示 `.gtl` 内嵌图标；构建期给 `SAMPLE.GTL` 写入示例图标。

## 2. 非目标

- 不引入真正的多进程多窗口；FILES/SYS 为桌面浮层窗口（与 Settings/About 同级）。
- 不实现文件双击运行/编辑；不改变 shell 命令语义。
- 不修改系统调用号/ABI；不改变既有测试断言的坐标、配色与文案。
- 不新增桌面图标（仍是 TERM/FILES/SYS 三枚），避免破坏既有测试布局。

## 3. 关键设计决策

### 3.1 闪烁根因与修复

`gui_leave()` 当前顺序为：`vga_hold_display(false)`（→ 视口内整屏 render + flip）→ 恢复鼠标矩形
（把旧桌面内容写回显存）→ `vga_gui_leave()`（→ 全屏 render + flip）。两次提交 + 一次局部回滚 =
可见闪烁。`gui_canvas_end()` 同样先 `vga_hold_display(false)` 全屏绘制控制台再重绘桌面，也会闪。

修复：
- `kernel/vga.c` 新增 `void vga_hold_display_quiet(bool on)`，只改 `disp_hold`，不 render。
- `gui_leave()`：先恢复/清除鼠标，`vga_hold_display_quiet(false)`，再 `vga_gui_leave()`（唯一一次
  全屏 render+flip），最后 `kprintf("[gui] text mode\n")`。
- `gui_canvas_end()`：`vga_hold_display_quiet(false)` 后直接 `redraw_desktop()`（一次 flip）。
- `gt.h` 增加 `vga_hold_display_quiet` 声明。

### 3.2 OOBE 默认值

- 文本 `oobe.c` 的 `ask_line()`：空回车 = 提交（返回 0），由调用方采用默认值；
  `ask_choice()`：空回车返回下界（语言默认 1=English）。
- 图形 `gscreen.c` 的 `gs_ask_line()`：空回车 = 提交；`gs_ask_choice()`：空回车返回 min。
- 主机名空 → 保留 `gcfg.hostname`；管理员空 → `admin`；口令空 → 无口令（已有行为）。
- 新增 i18n `oobe.defaults`（"Press Enter to accept defaults."），在欢迎语后打印/绘制一次；
  GUI 页脚改为 `Enter = OK / defaults    Ctrl-C = skip`。
- 串口兼容：所有既有 `L()` 文案与顺序不变；测试发送的显式值路径行为不变。

### 3.3 桌面交互模型（gui.c）

状态新增：
```c
static int  icon_px[3], icon_py[3]; static bool icon_pos_ready;   /* 图标位置 */
static bool icon_drag; static int icon_drag_idx, icon_dx, icon_dy, icon_moved;
static int  icon_last_click; static u32 icon_last_t;              /* 双击 */
static bool ctx_open; static int CTX_X,CTX_Y,CTX_W,CTX_H,CTX_IH,CTX_N,CTX_HOT,ctx_target;
static bool prop_open, files_open, sys_open;
static int PR_*, FL_*, SY_*, 对应关闭按钮/行矩形
static struct fat_dirent files_ents[12]; static int files_n, files_sel;
```

- `layout()`：首次用 ICON_X/Y/STEP 初始化 `icon_px/py`；新增属性/文件/系统窗口矩形。
- `draw_wallpaper()` / `icon_hit()`：使用 `icon_px/py`，支持任意拖动位置。
- 图标按下：记录拖动起点；移动超过 4px 标记 `icon_moved` 并实时改坐标+重绘；
  松开时若未移动且同图标 600ms 内二次点击 → `open_app(i)`，否则仅选中。
- `open_app(i)`：0=TERM（最小化则还原，关闭浮层）；1=FILES（`fat_list_dir("\\")` 载入并打开）；
  2=SYS（打开系统信息）；每次输出 `[gui] open <name>\n` 便于自动化断言。
- 拖入窗口：图标拖动松开时若指针在终端客户区 `CL_*` 内 → `kprintf("[gui] drop <name>\n")`
  并 `open_app(i)`。
- 右键：记录 `ctx_target`（-1=桌面，0..2=图标），打开上下文菜单（宽度按最长项计算，
  贴边自动翻转）；绘制在 `draw_overlays()` 最后。
  - 桌面项：Terminal / Files / System / Settings / Properties（5 项）
  - 图标项：Open / Properties（2 项）
  - 左键点项执行；点外部关闭；再次右键重新定位。
- 属性对话框：标题 + 键值行 + OK；桌面显示分辨率/运行时间/主题/磁盘可用/语言/版本；
  图标显示名称/类型/大小/位置。
- `gui_cursor_refresh()`：重画全部浮层，并按各浮层矩形 `gfx_fb_restore`（防止控制台提交遮住浮层）。
- `gui_enter/gui_leave/gui_canvas_begin` 重置/关闭全部新浮层与拖动状态。

### 3.4 FILES / SYS 窗口

- 共用外观：`C_FRAME` 底 + `C_TITLE` 标题栏 + 强调线 + 右上关闭按钮（复用 `draw_win_button`）。
- FILES：标题 `Files - C:\`；逐行显示名称/大小（目录显示 `<DIR>`）；`.gtl` 行左侧绘制内嵌图标
  （失败回退通用文件小图标）；目录绘制文件夹小图标；点击行选中高亮。
- SYS：标题 `System`；行内容：版本/内核、分辨率、运行时间、主题、语言、磁盘簇（总/空闲）。
- 均为模态浮层：点击关闭按钮或外部关闭；不占用终端视口。

### 3.5 `.gtl` 内嵌图标

格式扩展（向后兼容，仍以 `GTL1` 开头，`image_off = 16 + 28*count` 不变）：
```
[原 GTL1 头部 + 导出表][image(image_size)]
[可选] "GTI1" | u16 w | u16 h | RGBA w*h*4   (小端)
```
- `rust/gtcore/src/parse.rs`：`GtGtlInfo` 追加 `icon_off/icon_len/icon_w/icon_h`；
  校验尾部魔数与尺寸（1..256），无图标时置 0；新增单测（带图标/无图标/截断）。
- `kernel/gt.h`：同步结构体字段；新增 `bool lib_icon(int handle, const u8 **rgba, int *w, int *h);`
  与 `bool ico_draw_gtl_icon_at(const char *path,int x,int y,int w,int h);`
- `kernel/gtlib.c`：记录每库 icon 指针/尺寸；`lib_used` 覆盖图标区（避免被后续库覆盖）；
  实现 `lib_icon()`。
- `kernel/ico.c`：读 `.gtl` 到既有 `ico_buf`，解析 `GTI1`，面积平均缩放 + alpha 混合绘制。
- `build.py`：`build_demo_assets()` 给 `SAMPLE.GTL` 追加由 SVG 光栅化（32x32 RGBA）的图标；
  PIL 不可用时跳过（文件仍为合法无图标 GTL）。

## 4. 文件级变更清单

| 文件 | 变更 |
|------|------|
| `kernel/vga.c` | 新增 `vga_hold_display_quiet(bool)` |
| `kernel/gt.h` | 声明 `vga_hold_display_quiet`；`gt_gtl_info` 加图标字段；声明 `lib_icon`、`ico_draw_gtl_icon_at` |
| `kernel/gui.c` | 图标拖动/双击、右键菜单、属性、FILES/SYS 窗口、拖放、切换闪烁修复、浮层刷新 |
| `kernel/gscreen.c` | `gs_ask_line/gs_ask_choice` 空回车默认；页脚与默认提示 |
| `kernel/oobe.c` | `ask_line/ask_choice` 空回车默认；打印默认提示 |
| `kernel/i18n.c` | 新增 GUI 右键/属性/文件/系统/默认值文案（中英） |
| `kernel/gtlib.c` | 图标指针保存 + `lib_icon()` + `lib_used` 覆盖图标 |
| `kernel/ico.c` | `ico_draw_gtl_icon_at()` |
| `rust/gtcore/src/parse.rs` | 图标块解析 + 单测 |
| `build.py` | `SAMPLE.GTL` 追加内嵌图标 |
| `tools/test_gui.py` | 追加右键/属性/双击运行/图标拖动/拖入窗口断言（不改旧断言） |
| `README.md` / `README.zh-CN.md` | 桌面交互、默认安装、共享库图标说明；更新测试与尺寸 |

## 5. 验收标准

1. `python build.py` 通过，内核 ≤ 736 扇区。
2. `python -m pytest`/`cargo test`（rust/gtcore）通过，GTL 图标单测通过。
3. `tools/test_gui.py` 全部 PASS（含新增断言），`test_gui_lib.py`、`test_png.py`、
   `test_art.py` 不回归。
4. 点击 TEXT 返回文本：串口出现一次 `[gui] text mode`；截屏无中间态（人工确认）。
5. 双击 TERM/FILES/SYS 图标串口分别出现 `[gui] open terminal|files|system`。
6. 右键桌面出现菜单；Properties 打开属性对话框。
7. FILES 窗口可见 `SAMPLE.GTL` 行及其内嵌图标（非背景色像素）。
8. OOBE 全默认：连续回车可在语言/主机/管理员/口令步骤采用默认并完成安装。
9. README（中英）更新，与实现一致。
