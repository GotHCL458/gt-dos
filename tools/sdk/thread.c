/* GT-DOS 多线程示例: TC THREAD.C -> RUN THREAD.GTP
 * 子线程与主线程共享地址空间/全局变量; yield() 主动让出,
 * 定时器也会按时间片抢占 (系统调用期间不抢占). */
#include <stdio.h>

int counter;
int lock_id;

int worker(int id)
{
    int i;
    for (i = 0; i < 3; i++) {
        mutex_lock(lock_id);
        counter = counter + 1;
        mutex_unlock(lock_id);
        print_int(gettid());
        putc(' ');
        yield();
    }
    return id * 10;
}

int main(void)
{
    lock_id = mutex_create();
    int a = thread_create(worker, 1);
    int b = thread_create(worker, 2);
    printf("threads %d %d main %d\n", a, b, gettid());
    int ra = thread_join(a);
    int rb = thread_join(b);
    printf("join %d %d counter %d\n", ra, rb, counter);
    return 0;
}
