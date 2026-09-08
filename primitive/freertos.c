/**
 * freertos.c — FreeRTOS 风格 RTOS 内核实现
 *
 * 核心机制:
 *   1. 每任务独立栈 + ucontext 上下文; makecontext 创建, swapcontext 切换
 *   2. 8 级优先级就绪链表 (同优先级 FIFO → 轮转), 调度器总选最高优先级
 *   3. 单一"睡眠链表"统一管理所有阻塞 (延时/信号量/队列超时), 按唤醒时刻排序
 *   4. 每任务 wait_kind 标记等待对象类型, wake_task 精确摘链
 *   5. 互斥锁 + 优先级继承 (解决优先级反转)
 *   6. 软件定时器 (one-shot / 周期), 调度器空闲步进时触发
 *
 * 调度模型: 任务在阻塞点自动让出; 调度器空闲时推进 tick, 唤醒到期任务并
 * 重新仲裁最高优先级 → 等价于真 FreeRTOS 的 tick 级抢占 (SysTick ISR 版).
 */
/* ⚠️ feature-test 宏必须在任何系统头文件之前定义才生效:
 * - _GNU_SOURCE:      glibc(server/kickpi) + bionic(phone/Termux) 暴露 getcontext/makecontext/swapcontext
 * - _XOPEN_SOURCE 700: glibc 暴露 stack_t 类型 (否则 uc_stack 报 unknown type name)
 * - _DARWIN_C_SOURCE:  macOS (Darwin libc) 需要
 */
#define _GNU_SOURCE 1
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "freertos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Android(bionic) 不提供 getcontext/makecontext/swapcontext → 用自建 aarch64 汇编 shim */
#if defined(__ANDROID__) || defined(__BIONIC__)
#include "ctx_android.h"
#else
#include <ucontext.h>
#endif

/* ================================================================
 * 内部数据结构
 * ================================================================ */
#define MAX_TIMERS 16
#define MAX_SEMS   16
#define MAX_QUEUES 16

/* macOS 的 getcontext/swapcontext 会把 mcontext 内嵌写到
 * ucontext_t 结构体 +0x38..+0xC8 处 (uc_mcsize=0x2C8), 越界约 0x90 字节!
 * 若 ucontext_t 紧邻其他数据会踩坏邻居 (实测覆盖 g_tick/g_sleep_list)。
 * 因此每个 ucontext 后必须留足 padding。 */
typedef struct {
    ucontext_t uc;
    unsigned char _pad[0x300];
} UContext;

typedef struct {             /* 任务槽: 公开 TCB + 私有上下文 */
    TaskControlBlock tcb;
    UContext         uc;
} TaskSlot;

typedef struct { TaskControlBlock* head; TaskControlBlock* tail; } ReadyList;

typedef struct {             /* 信号量 */
    int      kind;           /* 0=binary 1=counting 2=mutex */
    uint32_t count;
    uint32_t max_count;
    TaskControlBlock* holder;    /* mutex 持有者 */
    uint8_t  orig_prio;          /* mutex 持有者原始优先级 */
    TaskControlBlock* waiters;
    int      valid;
} Sem_t;

typedef struct {             /* 队列 */
    uint32_t len, item_size, count, head;
    uint8_t* buf;
    TaskControlBlock* recv_waiters;
    TaskControlBlock* send_waiters;
    int      valid;
} Queue_t;

typedef struct {             /* 软件定时器 */
    char       name[12];
    TickType_t period;
    int        auto_reload;
    TickType_t expires;
    void*      id;
    TimerCallback_t cb;
    int        active;
    int        in_use;
} Timer_t;

static TaskSlot   g_slots[configMAX_TASKS];
static UContext   g_sched_ctx;          /* 调度器上下文 */
static TaskControlBlock* g_running = NULL;
static TickType_t g_tick = 0;
static int        g_scheduler_running = 0;

static ReadyList g_ready[configMAX_PRIORITIES];
static TaskControlBlock* g_sleep_list = NULL;
static Sem_t   g_sems[MAX_SEMS];
static Queue_t g_queues[MAX_QUEUES];
static Timer_t g_timers[MAX_TIMERS];

/* ================================================================
 * 链表工具
 * ================================================================ */
static void ready_append(ReadyList* rl, TaskControlBlock* t) {
    t->next = NULL;
    if (!rl->tail) { rl->head = rl->tail = t; }
    else { rl->tail->next = t; rl->tail = t; }
}
static TaskControlBlock* ready_pop(ReadyList* rl) {
    TaskControlBlock* t = rl->head;
    if (t) { rl->head = t->next; if (!rl->head) rl->tail = NULL; t->next = NULL; }
    return t;
}
static void ready_remove(ReadyList* rl, TaskControlBlock* target) {
    TaskControlBlock** pp = &rl->head; TaskControlBlock* prev = NULL;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            if (rl->tail == target) rl->tail = prev;
            target->next = NULL;
            return;
        }
        prev = *pp; pp = &(*pp)->next;
    }
}
static void add_to_ready(TaskControlBlock* t) { t->state = tskReady; ready_append(&g_ready[t->priority], t); }

static int in_list(TaskControlBlock* head, TaskControlBlock* t) {
    while (head) { if (head == t) return 1; head = head->next; }
    return 0;
}

/* 从睡眠链表 + 对象等待链表摘除, 置为就绪; result=唤醒原因 */
static void wake_task(TaskControlBlock* t, uint32_t result) {
    TaskControlBlock** pp = &g_sleep_list;                 /* 移出睡眠链表 */
    while (*pp) { if (*pp == t) { *pp = t->next; break; } pp = &(*pp)->next; }
    t->next = NULL;

    TaskControlBlock** wl = NULL;                          /* 移出对象等待链表 */
    switch (t->wait_kind) {
    case 1: { Sem_t* s = (Sem_t*)t->wait_object; wl = &s->waiters; } break;
    case 2: { Queue_t* q = (Queue_t*)t->wait_object; wl = &q->recv_waiters; } break;
    case 3: { Queue_t* q = (Queue_t*)t->wait_object; wl = &q->send_waiters; } break;
    }
    if (wl) { TaskControlBlock** p = wl; while (*p) { if (*p == t) { *p = t->next; break; } p = &(*p)->next; } }
    t->wait_object = NULL; t->wait_kind = 0;
    t->wait_result = result;
    if (t->state != tskDeleted) add_to_ready(t);
}

/* 把任务挂起: 加入睡眠链表 (按唤醒时刻排序) + 对象等待链表 */
static void block_on(TaskControlBlock* t, uint8_t kind, void* obj, TickType_t timeout) {
    t->state = tskBlocked;
    t->wait_kind = kind;
    t->wait_object = obj;
    t->wake_tick = (timeout == portMAX_DELAY) ? (TickType_t)-1 : g_tick + timeout;
    TaskControlBlock** pp = &g_sleep_list;                 /* 睡眠链表排序插入 */
    while (*pp && (*pp)->wake_tick <= t->wake_tick) pp = &(*pp)->next;
    t->next = *pp; *pp = t;

    TaskControlBlock** wl = NULL;                          /* 对象等待链表 */
    switch (kind) {
    case 1: { Sem_t* s = (Sem_t*)obj; wl = &s->waiters; } break;
    case 2: { Queue_t* q = (Queue_t*)obj; wl = &q->recv_waiters; } break;
    case 3: { Queue_t* q = (Queue_t*)obj; wl = &q->send_waiters; } break;
    }
    if (wl) { t->next = *wl; *wl = t; }                    /* 对象链表头插 */
}

/* ================================================================
 * 任务创建 / 删除
 * ================================================================ */
static void task_entry_scheduler(void) {
    TaskFunction_t fn = g_running->fn;
    void* arg = g_running->arg;
    fn(arg);                            /* 运行任务函数 */
    g_running->state = tskDeleted;      /* 函数返回 = 任务结束 */
    /* 返回 → uc_link = &g_sched_ctx, 自动回到调度器 */
}

TaskHandle_t xTaskCreate(TaskFunction_t fn, const char* name,
                         uint32_t stack_size, void* arg, uint8_t priority) {
    int i;
    for (i = 0; i < configMAX_TASKS; i++) if (!g_slots[i].tcb.in_use) break;
    if (i == configMAX_TASKS) { printf("[RTOS] 任务池已满!\n"); return NULL; }

    TaskControlBlock* t = &g_slots[i].tcb;
    memset(t, 0, sizeof(*t));
    if (stack_size == 0) stack_size = configMINIMAL_STACK;
    t->stack = malloc(stack_size);
    if (!t->stack) { printf("[RTOS] 栈分配失败!\n"); return NULL; }
    t->in_use = 1; t->fn = fn; t->arg = arg;
    t->priority = priority > configMAX_PRIORITIES - 1 ? configMAX_PRIORITIES - 1 : priority;
    t->state = tskReady; t->stack_size = stack_size;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "?");

    ucontext_t* uc = &g_slots[i].uc.uc;
    getcontext(uc);
    uc->uc_stack.ss_sp   = t->stack;
    uc->uc_stack.ss_size = t->stack_size;
    uc->uc_stack.ss_flags = 0;
    uc->uc_link          = &g_sched_ctx.uc;
    makecontext(uc, task_entry_scheduler, 0);
    t->ctx = uc;

    add_to_ready(t);
    return (TaskHandle_t)t;
}

void vTaskDelete(TaskHandle_t task) {
    TaskControlBlock* t = task ? (TaskControlBlock*)task : g_running;
    if (!t || t->state == tskDeleted) return;
    t->state = tskDeleted;
    if (t != g_running) {
        if (t->state == tskReady) ready_remove(&g_ready[t->priority], t);   /* 摘出就绪链表 */
        if (t->wait_object) {                                                /* 摘出睡眠/对象链表 */
            TaskControlBlock** pp = &g_sleep_list;
            while (*pp) { if (*pp == t) { *pp = t->next; break; } pp = &(*pp)->next; }
            TaskControlBlock** wl = NULL;
            switch (t->wait_kind) {
            case 1: { Sem_t* s = (Sem_t*)t->wait_object; wl = &s->waiters; } break;
            case 2: { Queue_t* q = (Queue_t*)t->wait_object; wl = &q->recv_waiters; } break;
            case 3: { Queue_t* q = (Queue_t*)t->wait_object; wl = &q->send_waiters; } break;
            }
            if (wl) { TaskControlBlock** p = wl; while (*p) { if (*p == t) { *p = t->next; break; } p = &(*p)->next; } }
            t->wait_object = NULL; t->wait_kind = 0;
        }
        if (t->stack) { free(t->stack); t->stack = NULL; }
        t->in_use = 0;
    }
    /* 删除自己: 由任务退出路径统一收尾 */
}

/* ================================================================
 * 延时 / 让出
 * ================================================================ */
static void yield_to_scheduler(void) {
    ucontext_t* uc = (ucontext_t*)g_running->ctx;
    swapcontext(uc, &g_sched_ctx.uc);
}

void vTaskDelay(TickType_t ticks) {
    if (g_running->state == tskDeleted) return;
    if (ticks == 0) { taskYIELD(); return; }
    block_on(g_running, 0, NULL, ticks);
    yield_to_scheduler();
}

void vTaskDelayUntil(TickType_t* prev, TickType_t incr) {
    if (g_running->state == tskDeleted) return;
    TickType_t target = *prev + incr;
    TickType_t wait = (TickType_t)(target - g_tick);
    if (wait > 0) { block_on(g_running, 0, NULL, wait); yield_to_scheduler(); }
    *prev = target;
}

void taskYIELD(void) {
    if (g_running->state == tskDeleted) return;
    ready_remove(&g_ready[g_running->priority], g_running);  /* 队首摘出 */
    add_to_ready(g_running);                                 /* 追加队尾 → 轮转 */
    yield_to_scheduler();
}

uint8_t uxTaskPriorityGet(TaskHandle_t task) {
    TaskControlBlock* t = task ? (TaskControlBlock*)task : g_running;
    return t ? t->priority : 0;
}
void vTaskPrioritySet(TaskHandle_t task, uint8_t prio) {
    TaskControlBlock* t = task ? (TaskControlBlock*)task : g_running;
    if (!t) return;
    if (t->state == tskReady) ready_remove(&g_ready[t->priority], t);
    t->priority = prio > configMAX_PRIORITIES - 1 ? configMAX_PRIORITIES - 1 : prio;
    if (t != g_running && t->state == tskReady) add_to_ready(t);
}
TickType_t xTaskGetTickCount(void) { return g_tick; }
const char* pcTaskGetName(TaskHandle_t task) {
    TaskControlBlock* t = task ? (TaskControlBlock*)task : g_running;
    return t ? t->name : "?";
}
uint32_t uxTaskGetNumberOfTasks(void) {
    uint32_t n = 0;
    for (int i = 0; i < configMAX_TASKS; i++) if (g_slots[i].tcb.in_use) n++;
    return n;
}

/* ================================================================
 * 信号量
 * ================================================================ */
static Sem_t* sem_alloc(int kind) {
    for (int i = 0; i < MAX_SEMS; i++)
        if (!g_sems[i].valid) { memset(&g_sems[i], 0, sizeof(Sem_t)); g_sems[i].valid = 1; g_sems[i].kind = kind; return &g_sems[i]; }
    printf("[RTOS] 信号量池满!\n"); return NULL;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    Sem_t* s = sem_alloc(0); if (!s) return NULL; s->count = 0; return s;
}
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t max_count, uint32_t init) {
    Sem_t* s = sem_alloc(1); if (!s) return NULL;
    s->max_count = max_count ? max_count : 1;
    s->count = init < s->max_count ? init : s->max_count;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    Sem_t* s = sem_alloc(2); if (!s) return NULL; s->count = 1; return s;
}

/* 互斥锁: 优先级继承 */
static void mutex_give_ml(Sem_t* s) {
    if (s->holder) {
        uint8_t op = s->holder->priority;
        s->holder->priority = s->orig_prio;               /* 恢复原始优先级 */
        if (s->holder != g_running && s->holder->state == tskReady) {
            if (s->holder->priority != op) {              /* 优先级变了才重排队 */
                ready_remove(&g_ready[op], s->holder);
                add_to_ready(s->holder);
            }
        }
    }
    s->holder = NULL; s->count = 1;
}
static void mutex_take_ml(Sem_t* s, TaskControlBlock* t) { s->holder = t; s->orig_prio = t->priority; s->count = 0; }
static void inherit_priority(Sem_t* s, TaskControlBlock* waiter) {
    TaskControlBlock* h = s->holder;
    if (!h || h == waiter) return;
    if (h->priority < waiter->priority) {
        uint8_t op = h->priority;
        if (h != g_running && h->state == tskReady) ready_remove(&g_ready[op], h);
        h->priority = waiter->priority;
        if (h != g_running && h->state == tskReady) add_to_ready(h);
    }
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout) {
    Sem_t* s = (Sem_t*)sem;
    if (!s || !s->valid || g_running->state == tskDeleted) return pdFAIL;
    if (s->kind == 2) {                                   /* 互斥锁 */
        if (s->count == 1) { mutex_take_ml(s, g_running); return pdTRUE; }
        inherit_priority(s, g_running);
        block_on(g_running, 1, s, timeout);
        yield_to_scheduler();
        return (g_running->wait_result == 1 && s->holder == g_running) ? pdTRUE : pdFAIL;
    }
    if (s->count > 0) { s->count--; return pdTRUE; }
    block_on(g_running, 1, s, timeout);
    yield_to_scheduler();
    return g_running->wait_result ? pdTRUE : pdFAIL;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    Sem_t* s = (Sem_t*)sem;
    if (!s || !s->valid) return pdFAIL;
    if (s->kind == 2) {                                   /* 互斥锁释放 */
        if (s->holder != g_running) return pdFAIL;
        mutex_give_ml(s);
        if (s->waiters) {                                 /* 直接把锁转给等待者 */
            TaskControlBlock* w = s->waiters;             /* 头插 → 取的是最后一个 */
            s->waiters = w->next;
            TaskControlBlock** pp = &s->waiters;          /* 摘出 w */
            while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
            w->next = NULL;
            wake_task(w, 1);
            mutex_take_ml(s, w);
        }
        return pdTRUE;
    }
    if (s->waiters) {                                     /* 有等待者: 直接唤醒 (计数不增) */
        TaskControlBlock* w = s->waiters;
        s->waiters = w->next;
        TaskControlBlock** pp = &s->waiters;              /* 摘出 w */
        while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
        w->next = NULL;
        wake_task(w, 1);
        return pdTRUE;
    }
    uint32_t cap = (s->kind == 1) ? s->max_count : 1;
    if (s->count < cap) s->count++;
    return pdTRUE;
}

/* ================================================================
 * 队列 (环形缓冲, 双端阻塞)
 * ================================================================ */
QueueHandle_t xQueueCreate(uint32_t len, uint32_t item_size) {
    for (int i = 0; i < MAX_QUEUES; i++) if (!g_queues[i].valid) {
        Queue_t* q = &g_queues[i];
        memset(q, 0, sizeof(*q));
        q->len = len; q->item_size = item_size;
        q->buf = malloc(len * item_size);
        if (!q->buf) return NULL;
        q->valid = 1;
        return (QueueHandle_t)q;
    }
    printf("[RTOS] 队列池满!\n"); return NULL;
}

BaseType_t xQueueSend(QueueHandle_t qh, const void* item, TickType_t timeout) {
    Queue_t* q = (Queue_t*)qh;
    if (!q || !q->valid || g_running->state == tskDeleted) return pdFAIL;
    if (q->count < q->len) {
        memcpy(q->buf + ((q->head + q->count) % q->len) * q->item_size, item, q->item_size);
        q->count++;
        if (q->recv_waiters) {                            /* 唤醒一个接收者 */
            TaskControlBlock* w = q->recv_waiters;
            q->recv_waiters = w->next;
            TaskControlBlock** pp = &q->recv_waiters;
            while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
            w->next = NULL;
            wake_task(w, 1);
        }
        return pdTRUE;
    }
    block_on(g_running, 3, q, timeout);                   /* 队列满: 阻塞发送 */
    yield_to_scheduler();
    if (g_running->wait_result == 1 && q->count < q->len) {
        memcpy(q->buf + ((q->head + q->count) % q->len) * q->item_size, item, q->item_size);
        q->count++;
        return pdTRUE;
    }
    return pdFAIL;
}

BaseType_t xQueueReceive(QueueHandle_t qh, void* buf, TickType_t timeout) {
    Queue_t* q = (Queue_t*)qh;
    if (!q || !q->valid || g_running->state == tskDeleted) return pdFAIL;
    if (q->count > 0) {
        memcpy(buf, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->len; q->count--;
        if (q->send_waiters) {                            /* 唤醒一个发送者 */
            TaskControlBlock* w = q->send_waiters;
            q->send_waiters = w->next;
            TaskControlBlock** pp = &q->send_waiters;
            while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
            w->next = NULL;
            wake_task(w, 1);
        }
        return pdTRUE;
    }
    block_on(g_running, 2, q, timeout);                   /* 队列空: 阻塞接收 */
    yield_to_scheduler();
    if (g_running->wait_result == 1 && q->count > 0) {
        memcpy(buf, q->buf + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1) % q->len; q->count--;
        if (q->send_waiters) {
            TaskControlBlock* w = q->send_waiters;
            q->send_waiters = w->next;
            TaskControlBlock** pp = &q->send_waiters;
            while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
            w->next = NULL;
            wake_task(w, 1);
        }
        return pdTRUE;
    }
    return pdFAIL;
}

/* ================================================================
 * 软件定时器
 * ================================================================ */
TimerHandle_t xTimerCreate(const char* name, TickType_t period,
                           BaseType_t auto_reload, void* id, TimerCallback_t cb) {
    for (int i = 0; i < MAX_TIMERS; i++) if (!g_timers[i].in_use) {
        Timer_t* tm = &g_timers[i];
        memset(tm, 0, sizeof(*tm));
        tm->period = period; tm->auto_reload = auto_reload;
        tm->id = id; tm->cb = cb; tm->in_use = 1;
        snprintf(tm->name, sizeof(tm->name), "%s", name ? name : "t");
        return (TimerHandle_t)tm;
    }
    printf("[RTOS] 定时器池满!\n"); return NULL;
}
BaseType_t xTimerStart(TimerHandle_t th, TickType_t block_time) {
    Timer_t* tm = (Timer_t*)th; (void)block_time;
    if (!tm || !tm->in_use) return pdFAIL;
    tm->active = 1; tm->expires = g_tick + tm->period;
    return pdTRUE;
}
BaseType_t xTimerStop(TimerHandle_t th, TickType_t block_time) {
    Timer_t* tm = (Timer_t*)th; (void)block_time;
    if (!tm || !tm->in_use) return pdFAIL;
    tm->active = 0;
    return pdTRUE;
}
static void timers_tick(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer_t* tm = &g_timers[i];
        if (!tm->in_use || !tm->active) continue;
        if ((TickType_t)(g_tick - tm->expires) < 0x80000000u) {   /* 已到期 */
            if (tm->cb) tm->cb(tm->id);
            if (tm->auto_reload) tm->expires += tm->period;
            else tm->active = 0;
        }
    }
}

/* ================================================================
 * 调度器
 * ================================================================ */
static TaskControlBlock* sched_pick(void) {
    for (int p = configMAX_PRIORITIES - 1; p >= 0; p--) {
        TaskControlBlock* t = ready_pop(&g_ready[p]);
        if (t) return t;
    }
    return NULL;
}

static void sys_tick(void) {
    g_tick++;
    TaskControlBlock* t = g_sleep_list;
    while (t && t->wake_tick != (TickType_t)-1 && t->wake_tick <= g_tick) {
        TaskControlBlock* next = t->next;
        uint32_t res = t->wait_object ? 0 : 1;            /* 对象等待 = 超时 */
        wake_task(t, res);
        t = next;
    }
    timers_tick();
}

static int count_active(void) {
    int n = 0;
    for (int i = 0; i < configMAX_TASKS; i++)
        if (g_slots[i].tcb.in_use && g_slots[i].tcb.state != tskDeleted) n++;
    return n;
}

void vTaskStartScheduler(void) {
    g_tick = 0;
    g_scheduler_running = 1;
    getcontext(&g_sched_ctx.uc);
    printf("[RTOS] ==== 调度器启动 (tick=0) ====\n");
    fflush(stdout);
    uint32_t guard = 0;
    while (g_scheduler_running) {
        TaskControlBlock* t = sched_pick();
        if (t) {
            g_running = t;
            t->state = tskRunning;
            t->runs++;
            ucontext_t* uc = (ucontext_t*)t->ctx;
            swapcontext(&g_sched_ctx.uc, uc);         /* 切到任务 */
            if (g_running && g_running->state == tskDeleted) {   /* 任务结束 */
                if (g_running->stack) { free(g_running->stack); g_running->stack = NULL; }
                g_running->in_use = 0;
                g_running = NULL;
            }
            continue;
        }
        sys_tick();                                        /* 无就绪任务 → 推进时间 */
        guard++;
        if (count_active() == 0) {
            g_scheduler_running = 0;
            printf("[RTOS] 所有任务已结束, 调度器停止 (tick=%u)\n", g_tick);
            break;
        }
        if (guard > 1000000u) {
            g_scheduler_running = 0;
            printf("[RTOS] !! 超过 100 万 tick 无进展, 疑似死锁/永久阻塞, 强制停止\n");
            break;
        }
    }
}

void vTaskStopScheduler(void) { g_scheduler_running = 0; }

/* ================================================================
 * 调试
 * ================================================================ */
void vTaskDumpReady(void) {
    printf("    就绪链表 @tick=%u:\n", g_tick);
    for (int p = configMAX_PRIORITIES - 1; p >= 0; p--) {
        TaskControlBlock* t = g_ready[p].head;
        if (t) {
            printf("      P%d:", p);
            while (t) { printf(" [%s r%u]", t->name, t->runs); t = t->next; }
            printf("\n");
        }
    }
    if (g_running) printf("    运行中: %s (P%d)\n", g_running->name, g_running->priority);
}
