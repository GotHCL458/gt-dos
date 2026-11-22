/* GT-DOS SDK: C GUI 库
 *
 * 用法 (在图形桌面里的 Shell 中运行):
 *     #include <gui.h>
 *     struct gt_gui_info gi;
 *     struct gt_gui_event ev;
 *     gfx_info(&gi);
 *     gui_begin();                 // 接管全屏 (失败: 未进入桌面)
 *     gfx_clear(0x101a30);
 *     gt_panel(40, 40, gi.w - 80, 200, "Demo");
 *     gt_button(60, 90, 120, 32, "OK", 1);
 *     gui_flush();
 *     gui_poll(&ev);               // ev.x/ev.y/ev.buttons/ev.key
 *     gui_end();                   // 恢复桌面
 *
 * 底层 gfx_* / gui_* 由 TC 直接编成 INT 0x21 系统调用;
 * 组合控件 (gt_*) 由 gui.c 模块在 #include <gui.h> 时编入程序.
 */
#ifndef _GT_GUI_H
#define _GT_GUI_H

/* 字段用 long: TC 的整数对象统一为 8 字节, 与内核 ABI 对齐 */
struct gt_gui_info  { long w, h, scale, cw, ch; };
struct gt_gui_event { long x, y, buttons, key; };

/* 默认配色 */
#define GT_BG     0x101a30
#define GT_PANEL  0x1b2434
#define GT_TITLE  0x1f4e8a
#define GT_TEXT   0xe8f0ff
#define GT_MUTED  0x8fa8c8
#define GT_ACCENT 0x66ccff
#define GT_BTN    0x2f5a8a
#define GT_OK     0x2f8f5f

/* 系统调用包装 (SYS_GFX_* / SYS_GUI_*) */
int  gfx_info(struct gt_gui_info *info);                  /* 64 */
int  gui_begin(void);                                     /* 65 */
int  gui_end(void);                                       /* 66 */
int  gfx_clear(unsigned color);                           /* 70 */
int  gfx_fill(int x, int y, int w, int h, unsigned color); /* 67 */
int  gfx_frame(int x, int y, int w, int h, unsigned color);/* 68 */
int  gfx_text(int x, int y, char *s, unsigned color);      /* 69 */
int  gfx_bar(int x, int y, int w, int h, int pct, unsigned color); /* 73 */
int  gui_flush(void);                                     /* 71 */
int  gui_poll(struct gt_gui_event *ev);                   /* 72 */

/* 组合控件 (gui.c) */
int  gt_panel(int x, int y, int w, int h, char *title);
int  gt_button(int x, int y, int w, int h, char *label, int hot);
int  gt_checkbox(int x, int y, char *label, int on);
int  gt_radio(int x, int y, char *label, int on);
int  gt_progress(int x, int y, int w, int h, int pct);
int  gt_hit(int x, int y, int w, int h, struct gt_gui_event *ev);

#endif /* _GT_GUI_H */
