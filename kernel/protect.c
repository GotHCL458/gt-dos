/* 内核容器与防篡改.
   内核镜像在链接期以 .container 节收尾 (位于 .data 之后、.bss 之前),
   build.py 在该位置写入 48 字节容器尾部并计算摘要. 启动早期由 Rust 侧
   SHA-256 / CRC32 校验载入内存的镜像, 任一字节被篡改都会 panic,
   不进入后续初始化.

   运行时防护分两层:
     1) 入口代码把 [0x10000, __ro_end) 的 4KB 页清除 RW 位, 代码与只读数据
        在内核态也不可写 (CR0.WP 已置位);
     2) 空闲时每 5 秒对同一区间重算 CRC32, 与构建期写入 .romark 的值比对,
        不一致立即 panic.

   .container 尾部格式 (小端):
     0   char[4] "GTK1"
     4   u32 version (1)
     8   u32 image_len (不含尾部)
     12  u32 crc32 (image_len 字节)
     16  u8[32]  sha256(image_len 字节)

   .romark 运行时校验区格式 (小端):
     0   char[4] "GTR1"
     4   u32 length (== __ro_end - 0x10000, 4KB 对齐)
     8   u32 crc32 (length 字节)
     12  u32 reserved */
#include "gt.h"

#define KERNEL_BASE    0x10000ULL
#define KERNEL_TRAILER 48
#define KERNEL_MAX     0x40000u          /* 512 扇区上限 */

/* 构建期标记为 "GTK0" (build.py 据此定位), 运行时变为 "GTK1" */
struct kernel_container {
    u8  magic[4];
    u32 version;
    u32 image_len;
    u32 crc;
    u8  sha256[32];
};

struct kernel_container __gt_container
    __attribute__((section(".container"), used, aligned(16))) = {
    { 'G', 'T', 'K', '0' }, 0, 0, 0, { 0 }
};

/* 运行时校验区: 位于只读段边界 (.romark), build.py 写入长度与 CRC32 */
struct kernel_runtime {
    u8  magic[4];
    u32 length;
    u32 crc;
    u32 reserved;
};

struct kernel_runtime __gt_runtime
    __attribute__((section(".romark"), used, aligned(16))) = {
    { 'G', 'T', 'R', '1' }, 0, 0, 0
};

static u32  g_image_len;
static u32  g_crc;
static u8   g_digest[32];
static bool g_verified;

static bool digest_eq(const u8 *a, const u8 *b)
{
    u8 d = 0;
    for (int i = 0; i < 32; i++)
        d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

/* 返回 0=通过, 负值为失败阶段 (供启动日志/调试) */
int kernel_self_check(void)
{
    const struct kernel_container *tr = &__gt_container;
    if (!(tr->magic[0] == 'G' && tr->magic[1] == 'T' &&
          tr->magic[2] == 'K' && tr->magic[3] == '1'))
        return -2;

    u32 ilen = tr->image_len;
    u32 crc = tr->crc;
    if (ilen == 0 || ilen != (u32)((const u8 *)tr - (const u8 *)KERNEL_BASE))
        return -3;

    const u8 *img = (const u8 *)KERNEL_BASE;
    if (gt_sha256(img, ilen, g_digest) != 32)
        return -4;
    if (!digest_eq(g_digest, tr->sha256))
        return -5;
    if (gt_crc32(img, ilen) != crc)
        return -6;

    g_image_len = ilen;
    g_crc = crc;
    g_verified = true;
    return 0;
}

/* 运行时完整性: 校验只读段 [0x10000, __ro_end) 的 CRC32.
   .romark 位于 __ro_end (4KB 对齐), 其地址即校验长度. */
int kernel_reverify(void)
{
    const struct kernel_runtime *rt = &__gt_runtime;
    if (!(rt->magic[0] == 'G' && rt->magic[1] == 'T' &&
          rt->magic[2] == 'R' && rt->magic[3] == '1'))
        return -1;

    u32 len = rt->length;
    if (len == 0 || (len & 0xFFFu) != 0)
        return -2;
    if (len != (u32)((const u8 *)rt - (const u8 *)KERNEL_BASE))
        return -3;
    if (gt_crc32((const u8 *)KERNEL_BASE, len) != rt->crc)
        return -4;
    return 0;
}

/* 空闲钩子: 每 5 秒复核一次, 发现代码/只读数据被改写立即停机 */
void kernel_idle_check(void)
{
    static u32 last_ms;
    u32 now = timer_uptime_ms();
    if (now - last_ms < 5000u)
        return;
    last_ms = now;
    if (kernel_reverify() != 0)
        panic("kernel tampering detected");
}

bool kernel_protected_info(u32 *image_len, u32 *crc, u8 digest[32])
{
    if (!g_verified)
        return false;
    if (image_len) *image_len = g_image_len;
    if (crc) *crc = g_crc;
    if (digest) memcpy(digest, g_digest, 32);
    return true;
}
