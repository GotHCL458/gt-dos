/*
 * GT-DOS 系统头文件: 共享库 (.gtl) 装载
 * SYS_LIBLOAD = 17, SYS_LIBSYM = 18
 */
#ifndef _GT_GTLIB_H
#define _GT_GTLIB_H

#include "unistd.h"

/* 返回句柄 (>=1) 或 -1 */
static inline int gt_libload(const char *path)
{
    return (int)gt_syscall(17L, (long)path, 0, 0);
}

/* 返回符号地址, 未找到为 0 */
static inline void *gt_libsym(int handle, const char *name)
{
    return (void *)gt_syscall(18L, (long)handle, (long)name, 0);
}

#endif /* _GT_GTLIB_H */
