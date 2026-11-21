/* 程序执行模型: 从 FAT 载入平面二进制到 PROG_BASE 并跳转运行.
   程序经 INT 0x21 SYS_EXIT 回跳 shell. 运行于 ring0, 无隔离(教学用). */
#include "gt.h"

extern u64 exec_enter(u64 entry, u64 stack_top, u64 *ctx);
extern void exec_longjmp(u64 *ctx, u64 code);
extern char __highbss_end[];

static u64 exec_ctx[3];
static volatile bool exec_active;

bool exec_is_active(void) { return exec_active; }

static const char *base_name(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

static bool layout_ok(void)
{
    u64 hb = (u64)__highbss_end;
    return PROG_BASE > hb && (PROG_BASE + PROG_MAX) <= 0x800000ULL;
}

/* \GT-DOS\KERNEL.GTP 是内核镜像的受保护视图, 不能当普通程序跳转执行 */
static bool is_kernel_image(const char *abs)
{
    const char *b = base_name(abs);
    return fat_path_protected(abs) && !strncasecmp(b, "KERNEL.GTP", 10);
}

int exec_run(const char *abs)
{
    if (!layout_ok())
        return -7;
    struct fat_dirent d;
    if (!fat_find(abs, &d)) return -1;
    if (d.is_dir) return -2;
    if (d.size == 0 || d.size > PROG_MAX) return -3;
    if (exec_active) return -4;
    if (is_kernel_image(abs)) return -9;
    u32 got = 0;
    if (!fat_read_file(abs, (void *)PROG_BASE, PROG_MAX, &got)) return -5;
    if (got != d.size) return -6;

    /* .gtp 完整性: 与 exFAT 目录项中记录的明文 CRC32 比对 */
    struct gt_statx sx;
    if (fat_statx(abs, &sx) == 0 && sx.crc != 0 &&
        gt_gtp_verify((const u8 *)PROG_BASE, got, sx.crc) != 0)
        return -8;

    exec_active = true;
    lib_reset();                    /* 每个程序独立库命名空间 */
    bool sys = task_path_is_system(abs);
    int tid = task_register(base_name(abs), sys ? TASK_SYS : TASK_USR,
                            (d.size + 1023) / 1024);
    u64 code = exec_enter(PROG_BASE, PROG_STACK_TOP, exec_ctx);
    thread_reap_all();              /* 回收程序创建的所有线程 */
    task_end(tid);
    exec_active = false;
    return (int)code;
}

void exec_exit(int code)
{
    if (!exec_active) return;
    exec_longjmp(exec_ctx, (u64)code);
}
