/* GT-DOS 时间 (经 INT 0x21 系统调用) */
#ifndef _GT_TIME_H
#define _GT_TIME_H
struct gt_time { int year, month, day, hour, min, sec; };
static inline int gt_gettime(struct gt_time *t)
{
    long ret;
    __asm__ __volatile__("int $0x21" : "=a"(ret) : "a"(10L), "D"(t)
                         : "rcx", "r11", "memory");
    return (int)ret;
}
static inline unsigned gt_uptime_ms(void)
{
    unsigned long ret;
    __asm__ __volatile__("int $0x21" : "=a"(ret) : "a"(7L)
                         : "rcx", "r11", "memory");
    return (unsigned)ret;
}
#endif /* _GT_TIME_H */