# 任务清单：GUI 登录/OOBE + 完整 PNG 渲染

> 依据 `spec.md`。按顺序执行，每步完成后更新状态。

## T1. 基础图形/头文件准备
- [x] `kernel/gt.h`：新增 `gs_available/gs_oobe_run/gs_login_screen` 声明。
- [x] `kernel/gt.h`：新增 `struct png_info` 与 `png_info/png_draw/png_draw_file/png_draw_file_fit` 声明。
- [x] `kernel/gt.h`：新增 `void gfx_pixel_a(int,int,u32,u8);` 声明（gfx 区）。
- 验证：`python build.py` 仅头文件改动后仍可构建（可先不做，最后统一构建）。

## T2. gfx 层 alpha 混合
- [x] `kernel/gfx.c`：实现 `gfx_pixel_a()`（无 alpha=255 直接写，0 跳过，否则 8 位混合）。
- 验证：随 T3/T4 构建验证。

## T3. PNG 解码器 `kernel/png.c`
- [x] 实现块解析（IHDR/PLTE/tRNS/IDAT/IEND，忽略其它，大端读取）。
- [x] 实现 IDAT 串联字节读取器（跨多个 IDAT 块续读）。
- [x] 实现 puff 风格 inflate：存储块/固定 Huffman/动态 Huffman；`getbits`；规范 Huffman 构造/解码。
- [x] 实现过滤重建 0..4（bpp 计算）。
- [x] 实现像素取值：位深 1/2/4/8/16；颜色类型 0/2/3/4/6；tRNS。
- [x] 实现 Adam7 隔行（7 pass，独立 prev 行）。
- [x] 实现 `png_draw()`（1:1 alpha 混合到后台缓冲，裁剪）。
- [x] 实现 `png_draw_file()`（fat_find 取大小 + fat_read_file 读入 `png_file`）。
- [x] 实现 `png_draw_file_fit()`（居中；可整数倍最近邻放大；过大则 1:1 居中裁剪）。
- [x] 实现 `png_info()`（快速返回宽高/位深/类型/隔行）。
- 验证：单元级用 `PNGINFO` 打印各测试 PNG 头字段。

## T4. GUI 认证界面 `kernel/gscreen.c`
- [x] `gs_backdrop()`：优先 `WALLPAPER.PNG`，否则渐变背景。
- [x] `gs_card()` / `gs_lines()` / `gs_field_draw()`：面板、标题、正文、字段局部刷新。
- [x] `gs_read_field()`：`ui_read_key` 循环（ASCII/退格/回车/Ctrl-C/ESC/方向/Del），mask 掩码。
- [x] `gs_ask_line()` / `gs_ask_choice()`：与旧逻辑一致（空输入重问/数字与范围校验）。
- [x] `gs_oobe_run()`：三步流程 + 完成，文案/副作用与旧实现一致，`vga_hold_display` 包裹。
- [x] `gs_login_screen()`：5 次重试、空用户/无此用户/口令错误/欢迎，取消返回默认用户。
- 验证：启动截图可见 GUI；串口握手与现有测试一致。

## T5. 接入与命令
- [x] `kernel/oobe.c`：`oobe_run()` 顶部 `if (gs_available()) { gs_oobe_run(); return; }`。
- [x] `kernel/user.c`：`ui_login_screen()` 顶部 `if (gs_available()) return gs_login_screen();`。
- [x] `kernel/shell.c`：新增 `SHOW <png>` 与 `PNGINFO <png>`；HELP 中英文补充说明。

## T6. 构建资源 `build.py`
- [x] `C_SOURCES` 增加 `gscreen.c`、`png.c`。
- [x] 实现 `build_png_assets()`（纯 Python 生成 PNG：SAMPLE/RGB8/PAL8/GRAY/GA/RGB16/PAL1/ADAM7/WALLPAPER）。
- [x] 把生成物加入 `assets`（根目录明文）。

## T7. 测试
- [x] 新增 `tools/test_png.py`（OOBE/登录握手 + PNGINFO + SHOW 截屏像素断言 + ADAM7）。
- [x] 回归：`test_v03/v04/gui/gui_lib/kbd/syscall/tasks/net/perf/protect` 全 PASS。
- [x] 构建尺寸 ≤ 640 扇区。

## T8. 文档与提交
- [x] 更新 `README.md`、`README.zh-CN.md`（GUI 登录/OOBE、PNG、测试列表、内核尺寸）。
- [x] 提交并 push（网络失败重试并报告）。

## 依赖关系
- T3 依赖 T1/T2；T4 依赖 T1/T2/T3（背景图）；T5 依赖 T3/T4；T6 依赖 T3（格式）；T7 依赖全部。
- T8 最后。

## 风险与对策
- `.highbss` 超限：构建期 `ASSERT(__highbss_end <= 0x580000)` 会报错；若超限，优先降低
  `PNG_FILE_MAX`/`PNG_ROW_MAX` 或壁纸尺寸。
- 动态 Huffman/隔行边界：用多个测试 PNG（含 filter 0..4、位深 1/8/16、Adam7）覆盖。
- 桌面内 `login` 与 `gui_pump` 冲突：已用 `gs_available()`（`!gui_active()`）规避，桌面内登录走原终端。
- 串口兼容：严格复用 i18n 文案与输出顺序，禁止改动。
