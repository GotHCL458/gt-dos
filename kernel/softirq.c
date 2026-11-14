/* 软中断: 中断上下半分离. 硬中断(top-half)只取端口一个字节, 耗时处理挂到
   软中断队列; bottom-half 在非中断上下文执行: 主循环空闲时 (cpu_relax /
   ui_read_key) 自动派发, 派发时关中断保证原子, 一次跑完已置位队列. */
#include "gt.h"

#define SOFTIRQ_MAX 8

static struct {
    void (*fn)(void);
    volatile bool pending;
} si[SOFTIRQ_MAX];

static volatile int si_running;       /* 防重入 */

void softirq_init(void)
{
    for (int i = 0; i < SOFTIRQ_MAX; i++) {
        si[i].fn = 0;
        si[i].pending = false;
    }
    si_running = 0;
}

void softirq_set(int nr, void (*fn)(void))
{
    if (nr >= 0 && nr < SOFTIRQ_MAX)
        si[nr].fn = fn;
}

/* 中断上下文或进程上下文都可调用 */
void softirq_raise(int nr)
{
    if (nr >= 0 && nr < SOFTIRQ_MAX)
        si[nr].pending = true;
}

/* 派发所有挂起的软中断 (调用方需处于可开中断的上下文) */
void softirq_dispatch(void)
{
    if (si_running)
        return;
    si_running = 1;

    for (;;) {
        cpu_cli();
        int nr = -1;
        for (int i = 0; i < SOFTIRQ_MAX; i++) {
            if (si[i].pending) {
                si[i].pending = false;
                nr = i;
                break;
            }
        }
        cpu_sti();
        if (nr < 0)
            break;
        if (si[nr].fn)
            si[nr].fn();
    }

    si_running = 0;
}
