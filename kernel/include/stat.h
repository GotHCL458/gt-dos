/*
 * GT-DOS 系统头文件: 文件元数据 (SYS_STAT = 13)
 */
#ifndef _GT_STAT_H
#define _GT_STAT_H

#include "unistd.h"

typedef struct {
    unsigned size;      /* 字节数 */
    unsigned attr;      /* FAT 属性位 */
    unsigned is_dir;    /* 1 = 目录 */
} gt_stat_t;

static inline int gt_stat(const char *path, gt_stat_t *st)
{
    return (int)gt_syscall(13L, (long)path, (long)st, 0);
}

#endif /* _GT_STAT_H */
