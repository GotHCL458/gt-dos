/*
 * GT-DOS 系统头文件: 文件元数据 (SYS_STAT = 13)
 */
#ifndef _GT_STAT_H
#define _GT_STAT_H

#include "unistd.h"

typedef struct {
    unsigned size;      /* 字节数 */
    unsigned attr;      /* 属性位 */
    unsigned is_dir;    /* 1 = 目录 */
} gt_stat_t;

static inline int gt_stat(const char *path, gt_stat_t *st)
{
    return (int)gt_syscall(13L, (long)path, (long)st, 0);
}

/* GT-DOS exFAT 厂商扩展元数据 (SYS_STATX = 19) */
#define GT_XFLAG_ENCRYPTED 0x01

typedef struct {
    unsigned size;
    unsigned attr;
    unsigned is_dir;
    unsigned flags;     /* GT_XFLAG_* */
    unsigned version;
    unsigned crc;       /* 明文 CRC32 */
    char     owner[16];
    unsigned short mode;
    unsigned short reserved;
} gt_statx_t;

static inline int gt_statx(const char *path, gt_statx_t *st)
{
    return (int)gt_syscall(19L, (long)path, (long)st, 0);
}

#endif /* _GT_STAT_H */
