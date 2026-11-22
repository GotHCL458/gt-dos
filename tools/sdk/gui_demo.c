/* GT-DOS C GUI 库演示程序 (TC \SDK\GUI_DEMO.C)
   编译: TC \SDK\GUI_DEMO.C    运行: RUN GUI_DEMO.GTP
   演示面板/按钮/复选框/单选/进度条, 约 1 秒后自动恢复桌面. */
#include <stdio.h>
#include <gui.h>

int main(void)
{
    struct gt_gui_info gi;
    struct gt_gui_event ev;
    int x, y, w, h, i, by;

    if (gfx_info(&gi) != 0) {
        printf("gui: no graphics\n");
        return 1;
    }
    if (gui_begin() != 0) {
        printf("gui: run this from the desktop\n");
        return 1;
    }

    x = 40;
    y = 40;
    w = gi.w - 80;
    h = gi.ch * 8 + 80;
    by = y + gi.ch + 20;

    for (i = 0; i < 30; i++) {
        gfx_clear(GT_BG);
        gt_panel(x, y, w, h, "GT GUI library demo");
        gt_button(x + 20, by, 10 * gi.cw, gi.ch + 8, "Button", 1);
        gt_checkbox(x + 20, by + gi.ch + 20, "Checkbox", 1);
        gt_radio(x + 20, by + 2 * (gi.ch + 20), "Radio", 1);
        gt_progress(x + 20, by + 3 * (gi.ch + 20), w - 40, gi.ch,
                    i * 100 / 29);
        gfx_text(x + 20, by + 4 * (gi.ch + 20),
                 "Compiled by TC, drawn via GTAPI", GT_MUTED);
        gui_flush();
        gui_poll(&ev);
        sleep(30);
    }

    gui_end();
    printf("gui demo done (%dx%d)\n", gi.w, gi.h);
    return 0;
}
