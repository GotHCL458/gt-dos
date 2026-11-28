# 任务清单：桌面完善（无闪烁切换 / 可用应用 / 交互 / 共享库图标）

> 依据 `spec.md`。按顺序执行，每步完成后勾选并继续。

## T1. 修复 GUI→CLI 闪烁
- [x] `kernel/vga.c` 新增 `vga_hold_display_quiet(bool on)`（仅改 `disp_hold`，不 render）。
- [x] `kernel/gt.h` 声明该函数。
- [x] `kernel/gui.c`：`gui_leave()` 改为「恢复指针 → quiet(false) → `vga_gui_leave()` → 打印」，
      只保留一次全屏提交。
- [x] `kernel/gui.c`：`gui_canvas_end()` 用 quiet(false) 后直接 `redraw_desktop()`。
- 验证：构建通过；进入/退出桌面串口各出现一次提示。

## T2. OOBE 默认选项
- [x] `kernel/oobe.c`：`ask_line` 空回车提交；`ask_choice` 空回车返回下界。
- [x] `kernel/gscreen.c`：`gs_ask_line` / `gs_ask_choice` 同上；页脚改 `Enter = OK / defaults`。
- [x] `kernel/i18n.c`：新增 `oobe.defaults`（中英）。
- [x] 文本/图形 OOBE 欢迎后输出默认提示（不改变既有提示串顺序）。
- 验证：串口连续回车走完全默认流程；原显式输入流程仍通过 test_gui。

## T3. 桌面图标交互（拖动/双击/拖放）
- [x] `kernel/gui.c`：`icon_px/py` 位置数组，`layout()` 首次初始化，`draw_wallpaper/icon_hit` 使用。
- [x] 图标按下记录拖动；移动 >4px 更新位置并重绘；松开判断双击（600ms）调用 `open_app`。
- [x] `open_app(i)`：TERM 还原终端；FILES 载入根目录并打开；SYS 打开；串口输出 `[gui] open ...`。
- [x] 图标松开落在终端客户区：输出 `[gui] drop <name>` 并 `open_app(i)`。
- [x] `gui_enter/gui_leave/gui_canvas_begin` 重置图标位置与拖动状态。
- 验证：新增 test_gui 断言（拖动后位置变化、双击后串口 open、拖入窗口 drop）。

## T4. 右键菜单与属性
- [x] `kernel/gui.c`：右键（bit1）检测；`ctx_open/ctx_target/CTX_*`；绘制于 `draw_overlays()` 最后。
- [x] 菜单项：桌面 5 项（Terminal/Files/System/Settings/Properties），图标 2 项（Open/Properties）。
- [x] 点击项执行；点外部关闭；贴边翻转；`gui_cursor_refresh` 同步恢复菜单矩形。
- [x] 属性对话框 `prop_open`：桌面信息（分辨率/运行时间/主题/磁盘/语言/版本）与图标信息（名称/类型/大小/位置）。
- [x] 串口辅助事件：`[gui] menu`、`[gui] properties`（便于自动化）。
- 验证：新增 test_gui 右键菜单/属性断言。

## T5. FILES / SYS 应用窗口
- [x] `kernel/gui.c`：`files_open`、`sys_open`；共用标题栏 + 关闭按钮 + 模态命中。
- [x] FILES：`fat_list_dir("\\", ...)`；行显示名称/大小/`<DIR>`；点击选中；`.gtl` 行调用
      `ico_draw_gtl_icon_at`，失败回退通用图标；目录画文件夹图标。
- [x] SYS：版本/内核、分辨率、运行时间、主题、语言、磁盘簇（总/空闲）。
- [x] `gui_cursor_refresh` 覆盖新窗口；`gui_canvas_begin` 关闭。
- 验证：test_gui 双击 FILES/SYS 截图 + 串口断言；FILES 中 `.gtl` 行有非背景像素。

## T6. `.gtl` 内嵌图标
- [x] `rust/gtcore/src/parse.rs`：`GtGtlInfo` 加 `icon_off/icon_len/icon_w/icon_h`；
      解析尾部 `GTI1 + u16 w + u16 h + RGBA`；边界校验；单测（有/无/截断）。
- [x] `kernel/gt.h`：同步字段；声明 `lib_icon`、`ico_draw_gtl_icon_at`。
- [x] `kernel/gtlib.c`：保存图标指针/尺寸；`lib_used` 覆盖图标区；实现 `lib_icon()`。
- [x] `kernel/ico.c`：实现 `ico_draw_gtl_icon_at()`（读文件→解析→面积平均缩放 alpha 混合）。
- [x] `build.py`：`SAMPLE.GTL` 追加 32x32 RGBA 图标（PIL 不可用则跳过）。
- 验证：`cargo test -p gtcore`；FILES 中 SAMPLE.GTL 图标像素非背景。

## T7. i18n 与文档
- [x] `kernel/i18n.c` 补齐全部新增文案（中英，键名见 spec 3.3/3.4）。
- [x] 更新 `README.md`、`README.zh-CN.md`：桌面交互（双击/拖动/右键/属性/拖放）、
      OOBE 默认值、`.gtl` 图标、测试列表、内核尺寸。
- 验证：`LANG zh`/`LANG en` 下桌面文案切换正常。

## T8. 构建与回归
- [x] `python build.py` 成功且内核 ≤ 736 扇区。
- [x] `cargo test -p gtcore` 通过。
- [x] `python tools/test_gui.py` 全 PASS（旧断言 + 新断言）。
- [x] `python tools/test_gui_lib.py`、`test_png.py`、`test_art.py` 不回归。
- [x] 人工确认 TEXT 切换无闪烁、OOBE 全默认可完成。
- 交付：报告变更与测试结果（不提交/推送，除非用户明确要求）。

## 依赖关系
T3/T4/T5 依赖 T1；T5 的 `.gtl` 图标依赖 T6；T7 依赖 T1–T6；T8 最后。

## 风险与对策
- 内核体积：新增约 600 行 GUI/解析代码，预计 +6~10 扇区级；超限时优先精简 FILES/SYS 绘制。
- 既有测试坐标/配色：不改变窗口、菜单、面板几何；新浮层只在交互后出现。
- `.highbss`/库区：图标指针指向已读入的库区，`lib_used` 计入图标长度，避免覆盖。
- 右键/双击在 QEMU 监视器下不稳定：测试使用按下/松开间抖动（沿用现有 `click()` 模式）。
