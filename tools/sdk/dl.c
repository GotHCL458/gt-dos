/* GT-DOS 素材下载示例 (TC \SDK\DL.C, RUN DL.GTP)
 *
 * 读取当前目录 DL.CFG 中的 "host:port" (例如 10.0.2.2:25001),
 * 通过内置 HTTP GET 依次下载 icon.ico / image.png / logo.svg 到 C: 根目录.
 * 下载完成后可用 SHOW 查看:
 *   SHOW ICON.ICO / SHOW IMAGE.PNG / SHOW LOGO.SVG
 */
#include <tc.h>
#include <stdio.h>
#include <net.h>

int main(void)
{
    char cfg[80];
    int fd, n, i, r, ok;

    fd = open("DL.CFG", 0);
    if (fd < 0) {
        printf("DL: missing DL.CFG\n");
        return 2;
    }
    n = read(fd, cfg, 76);
    close(fd);
    if (n <= 0) {
        printf("DL: empty DL.CFG\n");
        return 3;
    }
    i = 0;
    while (i < n && cfg[i] != '\r' && cfg[i] != '\n' && cfg[i] != ' ')
        i++;
    cfg[i] = 0;
    printf("DL: server %s\n", cfg);

    ok = 1;
    r = http_get(cfg, "/icon.ico", "C:\\icon.ico");
    printf("icon.ico  -> %d\n", r);
    if (r <= 0) ok = 0;

    r = http_get(cfg, "/image.png", "C:\\image.png");
    printf("image.png -> %d\n", r);
    if (r <= 0) ok = 0;

    r = http_get(cfg, "/logo.svg", "C:\\logo.svg");
    printf("logo.svg  -> %d\n", r);
    if (r <= 0) ok = 0;

    if (ok)
        printf("DL: all downloaded\n");
    else
        printf("DL: download failed\n");
    return ok ? 0 : 1;
}
