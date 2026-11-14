/* 构建期系统文件加密工具: 从 stdin 读明文, 输出 "GTE1" + u32 明文长 + AES-128-CBC 密文.
   直接复用 kernel/crypto.c, 保证与内核使用同一份算法. */
#include "gt.h"
#include <stdio.h>

int main(void)
{
    static unsigned char in[1 << 20];
    static unsigned char out[1 << 20];

    size_t n = fread(in, 1, sizeof(in), stdin);
    if (n >= sizeof(in)) {
        fprintf(stderr, "input too large\n");
        return 1;
    }
    u32 clen = gt_encrypt((const u8 *)in, (u32)n, (u8 *)out);

    unsigned char hdr[8];
    hdr[0] = 'G'; hdr[1] = 'T'; hdr[2] = 'E'; hdr[3] = '1';
    hdr[4] = (unsigned char)(n & 0xFF);
    hdr[5] = (unsigned char)((n >> 8) & 0xFF);
    hdr[6] = (unsigned char)((n >> 16) & 0xFF);
    hdr[7] = (unsigned char)((n >> 24) & 0xFF);
    fwrite(hdr, 1, 8, stdout);
    fwrite(out, 1, clen, stdout);
    return 0;
}
