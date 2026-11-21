/* GT-DOS TC 标准库: net.c (按需编入)
   在 socket 内置函数之上提供常用的连接/收发辅助函数.
   包含 <net.h> 后才会编入程序. */

/* 点分十进制 IPv4 解析, 非法返回 0 */
int net_aton(char *s)
{
    int part[4];
    int n = 0, v = 0, digits = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            digits = 1;
        } else if (c == '.') {
            if (!digits || n >= 3)
                return 0;
            part[n] = v;
            n++;
            v = 0;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (!digits || n != 3)
        return 0;
    part[3] = v;
    for (int k = 0; k < 4; k++)
        if (part[k] < 0 || part[k] > 255)
            return 0;
    return ip4(part[0], part[1], part[2], part[3]);
}

/* 解析主机名 (支持点分十进制或 DNS), 返回 IPv4 或 0 */
int net_resolve(char *host)
{
    int ip = net_aton(host);
    if (ip != 0)
        return ip;
    return dns(host);
}

/* 解析主机名并建立 TCP 连接, 失败返回 -1 */
int net_connect(char *host, int port)
{
    int ip = net_resolve(host);
    if (ip == 0)
        return -1;
    return tcp_connect(ip, port);
}

/* 发送 C 字符串 (自动计算长度) */
int net_send_str(int fd, char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return tcp_send(fd, s, n);
}

/* 发送全部字节, 返回实际发送数 */
int net_send_all(int fd, char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = tcp_send(fd, buf + off, len - off);
        if (n <= 0)
            return off;
        off = off + n;
    }
    return off;
}

/* 读取一行 (含换行符, 末尾补 NUL), 返回读取字节数 */
int net_recv_line(int fd, char *buf, int cap)
{
    int i = 0;
    while (i + 1 < cap) {
        char c;
        int n = tcp_recv(fd, &c, 1);
        if (n <= 0)
            break;
        buf[i] = c;
        i++;
        if (c == '\n')
            break;
    }
    buf[i] = 0;
    return i;
}

/* 读取到缓冲区满或对端关闭, 返回读取字节数 */
int net_recv_all(int fd, char *buf, int cap)
{
    int off = 0;
    while (off < cap) {
        int n = tcp_recv(fd, buf + off, cap - off);
        if (n <= 0)
            break;
        off = off + n;
    }
    return off;
}
