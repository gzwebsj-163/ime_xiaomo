/**
 * freertos.h — 阶段五: FreeRTOS 风格 RTOS 内核 (mini FreeRTOS)
 *
 * 在宿主 (macOS/Linux) 上用一个真实上下文切换内核复刻 FreeRTOS 的核心语义:
 *   - 任务: 创建/删除/优先级/阻塞/挂起
 *   - 调度: 8 级优先级就绪链表 + 同优先级轮转 + 延迟链表 + 优先级抢占
 *   - 同步: 二值信号量 / 计数信号量 / 互斥锁(带优先级继承)
 *   - 通信: 消息队列 (环形缓冲, 收发双向阻塞)
 *   - 时间: 软件定时器 (one-shot / 周期), vTaskDelayUntil 精确周期
 *
 * 上下文切换用 POSIX ucontext (每任务独立栈), 由宿主模拟时钟驱动。
 * 真 FreeRTOS 的抢占是 SysTick 定时器中断驱动的 (见阶段一 interrupt.c 的
 * 定时器中断), 本模拟中调度器推进 tick 时执行同样的"延迟唤醒+优先级仲裁"。
 *
 * 所有 API 命名/语义与 FreeRTOS 一一对应, 便于将来直移真内核。
 */
#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- 基础类型 (对齐 FreeRTOS) ---- */
typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef void*    TaskHandle_t;
typedef void*    SemaphoreHandle_t;
typedef void*    QueueHandle_t;
typedef void*    TimerHandle_t;
typedef struct   tskTaskControlBlock* TCB_t;

#define pdTRUE            1
#define pdFALSE           0
#define pdPASS            pdTRUE
#define pdFAIL            pdFALSE
#define portMAX_DELAY     ((TickType_t)0xFFFFFFFFu)

/* ---- 配置 ---- */
#define configMAX_PRIORITIES   8   /* 优先级 0~7, 7 最高 */
#define configMAX_TASKS       24   /* 静态任务池大小 */
#define configMINIMAL_STACK   (32*1024)  /* 每任务默认栈 (字节) */
#define configMAX_QUEUE_ITEMS 32   /* 队列可排队任务上限 */

/* ---- 任务函数原型 ---- */
typedef void (*TaskFunction_t)(void* arg);
typedef void (*TimerCallback_t)(void* timer_id);

/* ---- 任务状态 ---- */
typedef enum {
    tskReady    = 0,
    tskRunning  = 1,
    tskBlocked  = 2,   /* 阻塞在延时/信号量/队列 */
    tskDeleted  = 3
} eTaskState;

/* ---- 任务控制块 (TCB) ---- */
typedef struct tskTaskControlBlock {
    char        name[12];
    uint8_t     priority;
    uint8_t     state;            /* eTaskState */
    TaskFunction_t fn;
    void*       arg;
    void*       stack;            /* 任务栈 (malloc) */
    uint32_t    stack_size;
    void*       wait_object;      /* 等待的信号量/队列 */
    uint8_t     wait_kind;        /* 0=none 1=sem 2=queue_recv 3=queue_send */
    uint32_t    wait_result;      /* 被唤醒原因: 1=外部唤醒, 0=超时 */
    TickType_t  wake_tick;        /* 延迟/超时唤醒时刻 */
    uint32_t    runs;             /* 被调度次数 */
    uint32_t    total_ticks;      /* 累计运行 tick */
    void*       ctx;              /* 内部 ucontext 指针 */
    struct tskTaskControlBlock* next;  /* 链表 */
    int         in_use;
} TaskControlBlock;

/* ================================================================
 * 任务 API
 * ================================================================ */
TaskHandle_t xTaskCreate(TaskFunction_t fn, const char* name,
                         uint32_t stack_size, void* arg, uint8_t priority);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t* prev_wake, TickType_t incr);
void taskYIELD(void);
uint8_t uxTaskPriorityGet(TaskHandle_t task);
void vTaskPrioritySet(TaskHandle_t task, uint8_t prio);
TickType_t xTaskGetTickCount(void);
const char* pcTaskGetName(TaskHandle_t task);

/* ================================================================
 * 信号量 (二值 / 计数 / 互斥)
 * ================================================================ */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(uint32_t max_count, uint32_t init_count);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);

/* ================================================================
 * 队列
 * ================================================================ */
QueueHandle_t xQueueCreate(uint32_t len, uint32_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t q, void* buf, TickType_t timeout);

/* ================================================================
 * 软件定时器
 * ================================================================ */
TimerHandle_t xTimerCreate(const char* name, TickType_t period,
                           BaseType_t auto_reload, void* id, TimerCallback_t cb);
BaseType_t xTimerStart(TimerHandle_t t, TickType_t block_time);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t block_time);

/* ================================================================
 * 调度器
 * ================================================================ */
void vTaskStartScheduler(void);   /* 运行直到所有任务结束且无未来事件 */
void vTaskStopScheduler(void);    /* 强制停止 */

/* 调试辅助 */
void vTaskDumpReady(void);        /* 打印各优先级就绪队列 */
uint32_t uxTaskGetNumberOfTasks(void);

#endif /* FREERTOS_H */
