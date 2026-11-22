/* GT-DOS C GUI 库单元测试 (\SDK\GUI_UT.C)
   编译: TC \SDK\GUI_UT.C     运行: RUN GUI_UT.GTP
   自检返回值与 gt_hit 逻辑, 并绘制固定图案供宿主脚本按坐标核对像素.
   末尾 sleep 3.5 秒, 让截图工具在画布仍显示时抓屏. */
#include <stdio.h>
#include <gui.h>

int main(void)
{
    struct gt_gui_info gi;
    struct gt_gui_event ev;
    int rc, h1, h2, h3, h4;

    if (gfx_info(&gi) != 0) {
        printf("UT FAIL info\n");
        return 1;
    }
    printf("UT info %dx%d cw=%d ch=%d\n", gi.w, gi.h, gi.cw, gi.ch);

    if (gui_begin() != 0) {
        printf("UT NOGUI\n");
        return 1;
    }

    /* gt_hit: 区域内按下=1, 松开=0, 越界=0, 右下边界=1 */
    ev.x = 50; ev.y = 60; ev.buttons = 1; ev.key = -1;
    h1 = gt_hit(40, 40, 100, 50, &ev);
    ev.buttons = 0;
    h2 = gt_hit(40, 40, 100, 50, &ev);
    ev.buttons = 1; ev.x = 200;
    h3 = gt_hit(40, 40, 100, 50, &ev);
    ev.x = 139; ev.y = 89;
    h4 = gt_hit(40, 40, 100, 50, &ev);
    printf("UT hit %d%d%d%d\n", h1, h2, h3, h4);
    rc = 0;
    if (h1 != 1 || h2 != 0 || h3 != 0 || h4 != 1)
        rc = 1;

    /* 固定图案 (宿主按坐标核对) */
    gfx_clear(GT_BG);

    gt_panel(40, 40, 300, 160, "P");
    gt_button(360, 40, 100, 30, "B0", 0);
    gt_button(360, 80, 100, 30, "B1", 1);
    gt_checkbox(360, 120, "C", 1);
    gt_checkbox(360, 150, "C", 0);
    gt_radio(480, 40, "R", 1);
    gt_progress(480, 90, 200, 16, 50);

    gfx_fill(40, 220, 50, 20, 0xff0000);
    gfx_frame(40, 250, 50, 20, 0x00ff00);
    gfx_text(120, 220, "UT", 0xffff00);

    gui_flush();
    printf("UT drawn rc=%d\n", rc);
    sleep(3500);
    gui_end();
    printf("UT done\n");
    return rc;
}
