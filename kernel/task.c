/* 系统任务表: 内核服务与用户程序统一登记.
   SYS 任务代表内核/系统服务, 受保护, 不允许结束或删除;
   USR 任务由 RUN 在执行期间临时登记. 当前为单任务模型, 表用于展示与权限判定. */
#include "gt.h"

#define TASK_MAX 16

struct task_ent {
    bool used;
    u8   id;
    u8   type;
    u8   state;
    bool prot;                    /* SYS 任务受保护 */
    u32  mem_kb;
    char name[20];
    char owner[12];
};

static struct task_ent tasks[TASK_MAX];
static u8 last_id;

static void copy_str(char *dst, const char *src, int cap)
{
    int i = 0;
    if (src) {
        for (; src[i] && i < cap - 1; i++)
            dst[i] = src[i];
    }
    dst[i] = 0;
}

int task_register(const char *name, u8 type, u32 mem_kb)
{
    int slot = -1;
    for (int i = 0; i < TASK_MAX; i++)
        if (!tasks[i].used) { slot = i; break; }
    if (slot < 0)
        return -1;
    struct task_ent *e = &tasks[slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->id = ++last_id;
    e->type = type;
    e->state = TASK_RUN;
    e->prot = (type == TASK_SYS);
    e->mem_kb = mem_kb;
    copy_str(e->name, name, sizeof(e->name));
    copy_str(e->owner, type == TASK_SYS ? "SYSTEM" : user_name(),
             sizeof(e->owner));
    return e->id;
}

void task_end(int id)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && tasks[i].id == (u8)id) {
            tasks[i].used = false;
            return;
        }
}

static struct task_ent *task_find(int id)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && tasks[i].id == (u8)id)
            return &tasks[i];
    return 0;
}

int task_count(u8 type)
{
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].used && (type == 0xFF || tasks[i].type == type))
            n++;
    return n;
}

bool task_is_sys(int id)
{
    struct task_ent *e = task_find(id);
    return e && e->type == TASK_SYS;
}

bool task_path_is_system(const char *abs)
{
    if (!abs)
        return false;
    if (abs[0] && abs[1] == ':')
        abs += 2;
    return strncasecmp(abs, "\\gt-dos\\", 8) == 0;
}

/* 结束任务: SYS 一律拒绝; USR 在运行中也不能强杀 (单任务协作模型). */
int task_kill(int id)
{
    struct task_ent *e = task_find(id);
    if (!e)
        return -1;
    if (e->type == TASK_SYS)
        return -2;
    if (e->state == TASK_RUN)
        return -3;
    e->used = false;
    return 0;
}

/* 按名称停止任务 (忽略扩展名). SYS 受保护. */
int task_stop(const char *name)
{
    int nlen = 0;
    while (name[nlen] && name[nlen] != '.')
        nlen++;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task_ent *e = &tasks[i];
        if (!e->used)
            continue;
        int en = 0;
        while (e->name[en] && e->name[en] != '.')
            en++;
        if (en != nlen || strncasecmp(e->name, name, nlen) != 0)
            continue;
        if (e->type == TASK_SYS)
            return -2;
        if (e->state == TASK_RUN)
            return -3;
        e->used = false;
        return 0;
    }
    return -1;
}

const char *task_last_name(void)
{
    for (int i = TASK_MAX - 1; i >= 0; i--)
        if (tasks[i].used && tasks[i].type == TASK_USR)
            return tasks[i].name;
    return "";
}

void task_dump(void)
{
    kprintf("ID  TYPE  PROT  NAME              STATE  MEM    OWNER\n");
    for (int i = 0; i < TASK_MAX; i++) {
        struct task_ent *e = &tasks[i];
        if (!e->used)
            continue;
        char mem[12] = "-";
        if (e->mem_kb) {
            char num[10];
            itoa_base(e->mem_kb, num, 10, false);
            int k = 0;
            for (char *q = num; *q; q++)
                mem[k++] = *q;
            mem[k++] = 'K';
            mem[k] = 0;
        }
        kprintf("#%-2u  %-3s  %-4s  %-16s %-5s  %-4s  %s\n",
                (u32)e->id,
                e->type == TASK_SYS ? "SYS" : "USR",
                e->prot ? "yes" : "-",
                e->name,
                e->state == TASK_RUN ? "run" : "stop",
                mem, e->owner);
    }
    kprintf("%d system task(s), %d user task(s).\n",
            task_count(TASK_SYS), task_count(TASK_USR));
}

/* 启动时登记内核服务 (名称/内存占用仅用于展示) */
void task_init(void)
{
    memset(tasks, 0, sizeof(tasks));
    last_id = 0;
    task_register("kernel.core", TASK_SYS, 0);
    task_register("kernel.image", TASK_SYS, 0);   /* GTK1 受保护容器 */
    task_register("net.stack", TASK_SYS, 0);
    task_register("fs.exfat", TASK_SYS, 0);
    task_register("tty.console", TASK_SYS, 0);
    task_register("crypto.aes", TASK_SYS, 0);
}
