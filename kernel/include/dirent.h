/*
 * GT-DOS 系统头文件: 目录遍历 (SYS_OPENDIR = 14, SYS_READDIR = 15)
 */
#ifndef _GT_DIRENT_H
#define _GT_DIRENT_H

#include "unistd.h"

typedef struct {
    char     name[16];
    unsigned size;
    unsigned attr;
    unsigned is_dir;
} gt_dirent_t;

static inline int gt_opendir(const char *path)
{
    return (int)gt_syscall(14L, (long)path, 0, 0);
}

/* 1 = 取到一项, 0 = 结束, -1 = 错误 */
static inline int gt_readdir(int fd, gt_dirent_t *de)
{
    return (int)gt_syscall(15L, (long)fd, (long)de, 0);
}

static inline int gt_closedir(int fd)
{
    return (int)gt_syscall(6L, (long)fd, 0, 0);
}

#endif /* _GT_DIRENT_H */
