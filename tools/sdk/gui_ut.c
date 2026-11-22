/* GT-DOS C GUI 库单元测试 (\SDK\GUI_UT.C)
   编译: TC \SDK\GUI_UT.C     运行: RUN GUI_UT.GTP
   阶段 1: 返回值 + gt_hit 逻辑 + 固定图案 (宿主按坐标核对像素).
   阶段 2: 边界用例 (负坐标裁剪/零尺寸/百分比钳制/NULL 参数/重复调用).
   每个阶段末尾 sleep, 让截图工具在画布仍显示时抓屏. */
#include <stdio.h>
#include <gui.h>

int main(void)
{
    struct gt_gui_info gi;
    struct gt_gui_event ev;
    int rc, h1, h2, h3, h4;
    int r1, r2, r3, r4, r5, W, H;

    if (gfx_info(&gi) != 0) {
        printf("UT FAIL info\n");
        return 1;
    }
    printf("UT info %dx%d cw=%d ch=%d\n", gi.w, gi.h, gi.cw, gi.ch);

    if (gui_begin() != 0) {
        printf("UT NOGUI\n");
        return 1;
    }

    /* ---- 阶段 1: gt_hit 逻辑 ---- */
    ev.x = 50; ev.y = 60; ev.buttons = 1; ev.key = -1;
    h1 = gt_hit(40, 40, 100, 50, &ev);       /* 区域内按下 = 1 */
    ev.buttons = 0;
    h2 = gt_hit(40, 40, 100, 50, &ev);       /* 松开 = 0 */
    ev.buttons = 1; ev.x = 200;
    h3 = gt_hit(40, 40, 100, 50, &ev);       /* 越界 = 0 */
    ev.x = 139; ev.y = 89;
    h4 = gt_hit(40, 40, 100, 50, &ev);       /* 右下边界 x+w-1,y+h-1 = 1 */
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

    /* ---- 阶段 2: 边界用例 ---- */
    W = gi.w;
    H = gi.h;
    gfx_clear(GT_BG);

    /* 负坐标/越界裁剪: 只画可见部分 */
    gfx_fill(-10, -10, 20, 20, 0x112233);            /* 可见 10x10 于 (0,0) */
    gfx_fill(W - 10, H - 10, 20, 20, 0x445566);      /* 可见 10x10 于右下 */
    /* 零尺寸 = 空操作; 1x1 可画 */
    gfx_fill(100, 100, 0, 0, 0xff00ff);
    gfx_fill(200, 100, 1, 1, 0xff00ff);
    /* 极小边框: 1x1 不画, 2x2 画角 */
    gfx_frame(220, 100, 1, 1, 0x00ffff);
    gfx_frame(240, 100, 2, 2, 0x00ffff);
    /* 进度条百分比钳制: -20 -> 0, 150 -> 100 */
    gfx_bar(100, 160, 100, 16, -20, 0xff8800);
    gfx_bar(240, 160, 100, 16, 150, 0xff8800);
    /* 零宽零高进度条不应崩溃 */
    gfx_bar(100, 200, 0, 0, 50, 0xffffff);
    /* 负坐标文字只裁切可见部分 */
    gfx_text(-4, 220, "AB", 0xffff00);

    /* 组合控件钳制与 NULL 参数 */
    gt_progress(100, 240, 100, 16, -50);
    gt_progress(240, 240, 100, 16, 150);
    gt_button(400, 100, 80, 24, 0, 1);
    gt_checkbox(400, 140, 0, 1);
    gt_radio(400, 180, 0, 1);
    gt_panel(400, 220, 120, 60, 0);
    gt_checkbox(-5, 300, "N", 1);

    /* 参数校验: gui_poll(NULL) / gfx_info(NULL) 必须返回 -1;
       重复 gui_begin 幂等返回 0 */
    ev.buttons = 0; ev.key = -1;
    r1 = gui_poll(&ev);
    r2 = gui_poll(0);
    r3 = gfx_info(0);
    r4 = gui_begin();
    printf("UT edge rc=%d %d %d %d\n", r1, r2, r3, r4);
    if (r1 != 0 || r2 != -1 || r3 != -1 || r4 != 0)
        rc = 1;

    gui_flush();
    printf("UT edge drawn\n");
    sleep(3000);

    gui_end();
    r5 = gui_end();                          /* 重复 end = 空操作 */
    printf("UT edge rc2=%d\n", r5);
    if (r5 != 0)
        rc = 1;
    printf("UT edge done\n");
    return rc;
}
