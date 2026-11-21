/* 多线程调度: 轮转 + 定时器时间片抢占 + 主动 yield.
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
 * 系统调用执行期间 (sched_syscall_enter) 不响应定时抢占, 避免在文件系统/
 * 网络/控制台的临界区切走; yield/exit/join 仍可显式切换. */
#include "gt.h"

#define THREAD_STACK   8192
#define THREAD_QUANTUM 5                    /* 5 个 tick (约 50ms) */

enum { TH_FREE = 0, TH_READY, TH_RUN, TH_ZOMBIE };

struct thread {
    u8   state;
    bool is_main;
    int  id;
    int  lock;                              /* >0: 定时器不抢占 */
    u64  rsp;                               /* 保存的中断帧基址 */
    thread_fn_t fn;
    void *arg;
    i64  exit_code;
    u64  xmm[4][2];                         /* xmm0..3 */
    char name[16];
};

static struct thread th[THREAD_MAX];
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
    th_cur = 0;
    th_next_id = 2;
    th_slice = 0;
    sched_next_sp = 0;
    th[0].state = TH_RUN;
    th[0].is_main = true;
    th[0].id = 1;
    name_copy(th[0].name, "main");
}

void sched_syscall_enter(void)
{
    th[th_cur].lock++;
}

void sched_syscall_exit(void)
{
    if (th[th_cur].lock > 0)
        th[th_cur].lock--;
}

int sched_current_id(void) { return th[th_cur].id; }
bool sched_current_is_main(void) { return th[th_cur].is_main; }

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
    sched_xmm_save((u64 *)c->xmm);
    sched_xmm_load((const u64 *)n->xmm);
    sched_next_sp = n->rsp;
    n->state = TH_RUN;
    if (c->state == TH_RUN)
        c->state = TH_READY;
    th_cur = nx;
    th_slice = 0;
}

/* 定时器 tick: 时间片用完则轮转 (嵌套中断与系统调用中不抢占) */
void sched_tick(struct regs *r)
{
    if (th[th_cur].state != TH_RUN)
        return;
    if (irq_in_service() > 1)
        return;
    if (th[th_cur].lock > 0)
        return;
    th[th_cur].rsp = (u64)r;
    if (++th_slice < THREAD_QUANTUM)
        return;
    int nx = pick_next();
    if (nx < 0) {
        th_slice = 0;
        return;
    }
    switch_to(nx);
}

/* INT 0x2F: 主动让出 CPU */
void sched_yield_isr(struct regs *r)
{
    struct thread *c = &th[th_cur];
    if (c->state == TH_RUN)
        c->rsp = (u64)r;
    int nx = pick_next();
    if (nx < 0) {
        th_slice = 0;
        if (c->state == TH_ZOMBIE)
            panic("no runnable thread");
        return;
    }
    switch_to(nx);
}

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
    f.rsp = sp;                             /* iret 若带 SS:RSP 也能正确恢复 */
    f.ss = 0x10;
    memcpy((void *)fb, &f, sizeof(f));

    struct thread *t = &th[slot];
    memset(t, 0, sizeof(*t));
    t->state = TH_READY;
    t->id = th_next_id++;
    t->rsp = fb;
    t->fn = fn;
    t->arg = arg;
    name_copy(t->name, "worker");
    return t->id;
}

/* 当前线程退出: 标记僵尸并切走, 不再返回 */
void thread_exit_current(int code)
{
    struct thread *c = &th[th_cur];
    c->exit_code = code;
    c->state = TH_ZOMBIE;
    for (;;)
        __asm__ __volatile__("int $0x2f" ::: "memory");
}

/* 等待线程结束并回收; 返回其退出码 */
int thread_join(int tid)
{
    if (tid <= 1)
        return -1;
    for (;;) {
        int slot = -1;
        for (int i = 1; i < THREAD_MAX; i++)
            if (th[i].state != TH_FREE && th[i].id == tid) {
                slot = i;
                break;
            }
        if (slot < 0)
            return -1;
        if (th[slot].state == TH_ZOMBIE) {
            i64 code = th[slot].exit_code;
            th[slot].state = TH_FREE;
            th[slot].rsp = 0;
            return (int)code;
        }
        __asm__ __volatile__("int $0x2f" ::: "memory");
    }
}

/* 程序退出时回收其创建的全部线程 (当前线程必须是主线程) */
void thread_reap_all(void)
{
    if (!th[th_cur].is_main)
        return;
    for (int i = 1; i < THREAD_MAX; i++) {
        if (th[i].state != TH_FREE) {
            th[i].state = TH_FREE;
            th[i].rsp = 0;
        }
    }
}

static const char *state_name(u8 s)
{
    switch (s) {
    case TH_READY: return "ready";
    case TH_RUN:   return "run";
    case TH_ZOMBIE:return "zombie";
    default:       return "free";
    }
}

void sched_dump(void)
{
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
    kprintf("current thread: #%d, timer slice %d/%d\n",
            th[th_cur].id, th_slice, THREAD_QUANTUM);
}
