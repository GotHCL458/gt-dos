/* 多线程调度: 轮转 + 定时器时间片抢占 + 主动 yield + 阻塞等待.
 *
 * 线程都在 ring0、共享程序地址空间, 各自独立栈. 上下文切换统一走中断帧:
 *   - 定时器 IRQ 与 INT 0x2F (yield) 都由 isr_common 保存 15 个通用寄存器
 *     与 iret 帧; 调度器只需记录/替换栈指针, isr_common 从新帧恢复.
 *   - 新线程在自建栈上伪造同样的帧 (rip=thread_entry_asm, rdi=入口函数,
 *     rsi=参数), 由 iret 首次进入入口函数.
 *   - 入口函数按 TC 的调用约定调用: 参数压栈后再 call, 因此用户 TC 函数可用
 *     [rbp+16] 读取参数; 返回后自动 thread_exit_current(返回值).
 *   - xmm0..3 在切换时保存/恢复 (TC 的 double 运算使用 xmm).
 *
 * 阻塞: thread_join / mutex_lock / thread_sleep_ms 把线程置为 BLOCKED 后
 *     主动 yield; 定时器 tick 负责唤醒睡眠到期的线程, 线程退出/解锁唤醒
 *     等待者. 阻塞线程没有可运行线程时用 sti+hlt 等 tick.
 *
 * 系统调用执行期间 (sched_syscall_enter) 定时抢占被关闭, 避免在文件系统/
 * 网络/控制台的临界区切走; 显式 yield/阻塞仍然生效. */
#include "gt.h"

#define THREAD_STACK   12288
#define THREAD_QUANTUM 5                    /* 5 个 tick (约 50ms) */
#define MUTEX_MAX      16
#define STACK_GUARD    0x5A

enum { TH_FREE = 0, TH_READY, TH_RUN, TH_BLOCKED, TH_ZOMBIE };

struct thread {
    u8   state;
    bool is_main;
    bool detached;
    int  id;
    int  lock;                              /* >0: 定时器不抢占 */
    u64  rsp;                               /* 保存的中断帧基址 */
    thread_fn_t fn;
    void *arg;
    i64  exit_code;
    int  wait_next;                         /* 等待链表的下一项 */
    int  join_head;                         /* 等待本线程退出的线程链表 */
    int  wake_tick;                         /* 睡眠到期 tick (0=无) */
    u64  stack_base;                        /* 栈底 (溢出检测) */
    u64  xstate[17];                        /* xmm0..7 + mxcsr */
    char name[16];
};

struct mutex {
    bool used;
    int  owner;                             /* 线程槽位, -1=未持有 */
    int  wait_head;
};

static struct thread th[THREAD_MAX];
static struct mutex mx[MUTEX_MAX];
static u8 tstacks[THREAD_MAX - 1][THREAD_STACK]
    __attribute__((section(".highbss")));
static int th_cur;
static int th_next_id;
static int th_slice;

/* isr.asm 读取: 非 0 时用该栈帧恢复 (上下文切换) */
volatile u64 sched_next_sp;

extern void thread_entry_asm(void);
extern void sched_xmm_save(u64 *buf);
extern void sched_xmm_load(const u64 *buf);

static void name_copy(char *d, const char *s)
{
    int i = 0;
    if (s)
        for (; s[i] && i < 15; i++)
            d[i] = s[i];
    d[i] = 0;
}

void sched_init(void)
{
    memset(th, 0, sizeof(th));
    memset(mx, 0, sizeof(mx));
    for (int i = 0; i < THREAD_MAX; i++) {
        th[i].wait_next = -1;
        th[i].join_head = -1;
    }
    for (int i = 0; i < MUTEX_MAX; i++)
        mx[i].owner = -1;
    th_cur = 0;
    th_next_id = 2;
    th_slice = 0;
    sched_next_sp = 0;
    th[0].state = TH_RUN;
    th[0].is_main = true;
    th[0].id = 1;
    name_copy(th[0].name, "main");
}

void sched_syscall_enter(void) { th[th_cur].lock++; }
void sched_syscall_exit(void)
{
    if (th[th_cur].lock > 0)
        th[th_cur].lock--;
}

int sched_current_id(void) { return th[th_cur].id; }
bool sched_current_is_main(void) { return th[th_cur].is_main; }

/* ------------------------------------------------------------- 等待链表 */
static void wait_add(int *head, int idx)
{
    th[idx].wait_next = *head;
    *head = idx;
}

static int wake_one(int *head)
{
    while (*head >= 0) {
        int i = *head;
        *head = th[i].wait_next;
        th[i].wait_next = -1;
        if (th[i].state == TH_BLOCKED || th[i].state == TH_READY) {
            th[i].state = TH_READY;
            return i;
        }
    }
    return -1;
}

static void wake_all(int *head)
{
    int i = *head;
    *head = -1;
    while (i >= 0) {
        int nx = th[i].wait_next;
        th[i].wait_next = -1;
        if (th[i].state == TH_BLOCKED)
            th[i].state = TH_READY;
        i = nx;
    }
}

/* --------------------------------------------------------------- 调度 */
static int pick_next(void)
{
    for (int k = 1; k <= THREAD_MAX; k++) {
        int i = (th_cur + k) % THREAD_MAX;
        if (th[i].state == TH_READY)
            return i;
    }
    return -1;
}

/* 切换: 保存当前 xmm, 记录下一线程的栈帧地址, 由 isr.asm 恢复 */
static void switch_to(int nx)
{
    struct thread *c = &th[th_cur];
    struct thread *n = &th[nx];
    sched_xmm_save(c->xstate);
    sched_xmm_load(n->xstate);
    sched_next_sp = n->rsp;
    n->state = TH_RUN;
    if (c->state == TH_RUN)
        c->state = TH_READY;
    th_cur = nx;
    th_slice = 0;
}

/* 阻塞当前线程并让出; 返回后线程为 RUN (被唤醒) 或仍为 BLOCKED (无进展) */
static void block_yield(void)
{
    th[th_cur].state = TH_BLOCKED;
    __asm__ __volatile__("int $0x2f" ::: "memory");
    if (th[th_cur].state == TH_BLOCKED) {
        /* 没有可运行线程: 开中断等一个 tick (睡眠/退出可能到期) */
        cpu_sti();
        cpu_halt();
        cpu_cli();
        th[th_cur].state = TH_RUN;
    }
}

/* 定时器 tick: 唤醒到期睡眠 + 时间片轮转/切换 */
void sched_tick(struct regs *r)
{
    if (irq_in_service() > 1)
        return;

    /* 线程栈溢出检测 (主线程使用内核 shell 栈, 不检查) */
    if (!th[th_cur].is_main && th[th_cur].stack_base &&
        (u64)r < th[th_cur].stack_base + 256)
        panic("thread stack overflow");

    u32 now = timer_ticks();
    for (int i = 0; i < THREAD_MAX; i++) {
        if (th[i].state == TH_BLOCKED && th[i].wake_tick &&
            (i32)(now - (u32)th[i].wake_tick) >= 0) {
            th[i].wake_tick = 0;
            th[i].state = TH_READY;
        }
    }

    if (th[th_cur].state == TH_RUN) {
        if (th[th_cur].lock > 0)
            return;
        th[th_cur].rsp = (u64)r;
        if (++th_slice < THREAD_QUANTUM)
            return;
    } else if (th[th_cur].state == TH_BLOCKED ||
               th[th_cur].state == TH_READY) {
        /* 阻塞线程被唤醒或正在等 tick: 记录恢复点后切换 */
        th[th_cur].rsp = (u64)r;
    } else {
        /* ZOMBIE/已回收: 只为让其它线程运行而切换 */
        int nx = pick_next();
        if (nx >= 0)
            switch_to(nx);
        return;
    }

    int nx = pick_next();
    if (nx < 0) {
        th_slice = 0;
        return;
    }
    if (nx == th_cur) {
        th[th_cur].state = TH_RUN;
        th_slice = 0;
        return;
    }
    switch_to(nx);
}

/* INT 0x2F: 主动让出 CPU */
void sched_yield_isr(struct regs *r)
{
    struct thread *c = &th[th_cur];
    if (c->state == TH_RUN || c->state == TH_BLOCKED)
        c->rsp = (u64)r;
    int nx = pick_next();
    if (nx < 0) {
        th_slice = 0;
        return;
    }
    if (nx == th_cur) {
        c->state = TH_RUN;
        th_slice = 0;
        return;
    }
    switch_to(nx);
}

/* --------------------------------------------------------------- 线程 */
/* 新线程栈上伪造与 isr_common 相同的中断帧 */
int thread_create(thread_fn_t fn, void *arg)
{
    if (!fn)
        return -1;
    int slot = -1;
    for (int i = 1; i < THREAD_MAX; i++)
        if (th[i].state == TH_FREE) {
            slot = i;
            break;
        }
    if (slot < 0)
        return -2;

    u8 *base = tstacks[slot - 1];
    for (int i = 0; i < 8; i++)
        base[i] = STACK_GUARD;              /* 栈底哨兵 */
    u64 top = ((u64)base + THREAD_STACK) & ~15ULL;
    u64 sp = top - 8;                       /* 模拟 call 后的入口 rsp */
    u64 fb = sp - 176;                      /* 15 regs + int_no/err + iret 帧 */
    struct regs f;
    memset(&f, 0, sizeof(f));
    f.rdi = (u64)fn;
    f.rsi = (u64)arg;
    f.rip = (u64)thread_entry_asm;
    f.cs = 0x18;
    f.rflags = 0x202;                       /* IF=1 */
    f.rsp = sp;
    f.ss = 0x10;
    memcpy((void *)fb, &f, sizeof(f));

    struct thread *t = &th[slot];
    memset(t, 0, sizeof(*t));
    t->state = TH_READY;
    t->id = th_next_id++;
    t->rsp = fb;
    t->fn = fn;
    t->arg = arg;
    t->stack_base = (u64)base;
    t->wait_next = -1;
    t->join_head = -1;
    name_copy(t->name, "worker");
    return t->id;
}

/* 当前线程退出: 释放互斥量、唤醒 join 等待者, 然后切走不再返回 */
void thread_exit_current(int code)
{
    struct thread *c = &th[th_cur];
    c->exit_code = code;

    for (int i = 0; i < MUTEX_MAX; i++) {
        if (mx[i].used && mx[i].owner == th_cur) {
            mx[i].owner = -1;
            wake_one(&mx[i].wait_head);
        }
    }
    wake_all(&c->join_head);
    c->state = c->detached ? TH_FREE : TH_ZOMBIE;
    if (c->detached)
        c->rsp = 0;

    for (;;) {
        __asm__ __volatile__("int $0x2f" ::: "memory");
        /* 没有被切走: 等待 tick 唤醒睡眠者或让其它线程运行 */
        cpu_sti();
        cpu_halt();
        cpu_cli();
    }
}

/* 当前线程发生异常: 结束该线程并切换到其它线程 (由异常处理调用).
   主线程不适用 (由调用者判断). 返回后 isr_common 会恢复新线程的帧. */
void thread_abort_current(void)
{
    struct thread *c = &th[th_cur];
    if (c->is_main)
        return;
    c->exit_code = -1;
    for (int i = 0; i < MUTEX_MAX; i++) {
        if (mx[i].used && mx[i].owner == th_cur) {
            mx[i].owner = -1;
            wake_one(&mx[i].wait_head);
        }
    }
    wake_all(&c->join_head);
    c->state = c->detached ? TH_FREE : TH_ZOMBIE;
    c->rsp = 0;

    int nx = pick_next();
    if (nx < 0)
        panic("faulted thread: no runnable thread");
    sched_xmm_load(th[nx].xstate);
    sched_next_sp = th[nx].rsp;
    th[nx].state = TH_RUN;
    th_cur = nx;
    th_slice = 0;
}

static int find_by_tid(int tid)
{
    for (int i = 1; i < THREAD_MAX; i++)
        if (th[i].state != TH_FREE && th[i].id == tid)
            return i;
    return -1;
}

/* 等待线程结束并回收; 返回其退出码 */
int thread_join(int tid)
{
    if (tid <= 1 || tid == sched_current_id())
        return -1;
    bool queued = false;
    for (;;) {
        int slot = find_by_tid(tid);
        if (slot < 0 || th[slot].detached)
            return -1;
        if (th[slot].state == TH_ZOMBIE) {
            i64 code = th[slot].exit_code;
            th[slot].state = TH_FREE;
            th[slot].rsp = 0;
            return (int)code;
        }
        if (!queued) {
            wait_add(&th[slot].join_head, th_cur);
            queued = true;
        }
        block_yield();
    }
}

/* 分离线程: 退出时立即回收, 不再允许 join */
int thread_detach(int tid)
{
    if (tid <= 1)
        return -1;
    int slot = find_by_tid(tid);
    if (slot < 0)
        return -1;
    th[slot].detached = true;
    if (th[slot].state == TH_ZOMBIE) {
        th[slot].state = TH_FREE;
        th[slot].rsp = 0;
    }
    return 0;
}

/* 让出 CPU 一个时间片 */
void thread_yield(void)
{
    __asm__ __volatile__("int $0x2f" ::: "memory");
}

/* 睡眠 ms 毫秒; 只阻塞当前线程, 其它线程继续运行 */
void thread_sleep_ms(u32 ms)
{
    if (ms == 0) {
        thread_yield();
        return;
    }
    u32 hz = timer_hz();
    if (hz == 0)
        hz = 100;
    u32 ticks = (ms * hz + 999u) / 1000u;
    if (ticks == 0)
        ticks = 1;
    u32 deadline = timer_ticks() + ticks;
    th[th_cur].wake_tick = (int)deadline;
    while ((i32)(timer_ticks() - deadline) < 0) {
        block_yield();
    }
    th[th_cur].wake_tick = 0;
    th[th_cur].state = TH_RUN;
}

/* --------------------------------------------------------------- 互斥量 */
int mutex_create(void)
{
    for (int i = 0; i < MUTEX_MAX; i++) {
        if (!mx[i].used) {
            mx[i].used = true;
            mx[i].owner = -1;
            mx[i].wait_head = -1;
            return i + 1;
        }
    }
    return -1;
}

int mutex_lock(int id)
{
    if (id < 1 || id > MUTEX_MAX || !mx[id - 1].used)
        return -1;
    struct mutex *m = &mx[id - 1];
    for (;;) {
        if (m->owner < 0) {
            m->owner = th_cur;
            return 0;
        }
        if (m->owner == th_cur)
            return -2;                      /* 同一线程重复加锁 */
        wait_add(&m->wait_head, th_cur);
        block_yield();
    }
}

int mutex_unlock(int id)
{
    if (id < 1 || id > MUTEX_MAX || !mx[id - 1].used)
        return -1;
    struct mutex *m = &mx[id - 1];
    if (m->owner != th_cur)
        return -2;                          /* 非持有者 */
    m->owner = -1;
    wake_one(&m->wait_head);
    return 0;
}

int mutex_trylock(int id)
{
    if (id < 1 || id > MUTEX_MAX || !mx[id - 1].used)
        return -1;
    struct mutex *m = &mx[id - 1];
    if (m->owner < 0) {
        m->owner = th_cur;
        return 0;
    }
    return (m->owner == th_cur) ? -2 : -3;  /* -2 重入, -3 忙 */
}

int mutex_destroy(int id)
{
    if (id < 1 || id > MUTEX_MAX || !mx[id - 1].used)
        return -1;
    struct mutex *m = &mx[id - 1];
    if (m->owner >= 0 || m->wait_head >= 0)
        return -2;                          /* 仍被持有或有人等待 */
    m->used = false;
    return 0;
}

/* --------------------------------------------------------------- 回收 */
/* 程序退出时回收其创建的全部线程 (当前必须是主线程) */
void thread_reap_all(void)
{
    if (!th[th_cur].is_main)
        return;
    for (int i = 1; i < THREAD_MAX; i++) {
        if (th[i].state != TH_FREE) {
            th[i].state = TH_FREE;
            th[i].rsp = 0;
            th[i].wait_next = -1;
            th[i].join_head = -1;
        }
    }
    for (int i = 0; i < MUTEX_MAX; i++) {
        mx[i].used = false;
        mx[i].owner = -1;
        mx[i].wait_head = -1;
    }
    /* 程序可能忘记 critical_exit: 复位主线程的调度锁 */
    th[0].lock = 0;
    th[0].wake_tick = 0;
    th_next_id = 2;                     /* 每个程序的线程号从 2 开始 */
}

static const char *state_name(u8 s)
{
    switch (s) {
    case TH_READY:   return "ready";
    case TH_RUN:     return "run";
    case TH_BLOCKED: return "block";
    case TH_ZOMBIE:  return "zombie";
    default:         return "free";
    }
}

void sched_dump(void)
{
    int nmx = 0;
    for (int i = 0; i < MUTEX_MAX; i++)
        if (mx[i].used)
            nmx++;
    kprintf("ID  STATE   MAIN  NAME\n");
    for (int i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &th[i];
        if (t->state == TH_FREE)
            continue;
        kprintf("#%-2d %-7s %-5s %s%s\n",
                t->id, state_name(t->state),
                t->is_main ? "yes" : "-", t->name,
                (i == th_cur) ? "  *" : "");
    }
    kprintf("current #%d, slice %d/%d, %d mutex(es)\n",
            th[th_cur].id, th_slice, THREAD_QUANTUM, nmx);
}
