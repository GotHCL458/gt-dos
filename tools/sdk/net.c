#include "net.h"
#include "tc.h"

/* 连接 10.0.2.2:7777 (QEMU 用户网络中的宿主机), 发送一行并打印回显 */
int main(void)
{
    int fd = tcp_connect(ip4(10, 0, 2, 2), 7777);
    if (fd < 0) {
        print("CONNECT FAIL\n");
        return 1;
    }
    print("CONNECTED\n");
    tcp_send(fd, "hello\n", 6);

    int buf = 0x700000;              /* 程序镜像之上的空闲暂存区 */
    int n = tcp_recv(fd, buf, 64);
    if (n > 0)
        write(1, buf, n);
    tcp_close(fd);
    return 0;
}
