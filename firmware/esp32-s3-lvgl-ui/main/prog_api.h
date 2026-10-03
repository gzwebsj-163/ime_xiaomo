/**
 * prog_api.h — 烧录器后台（PROG 页的真后端）
 *
 * 职责：把 UI 上的一次「长按执行」变成一次**真的**工作：
 *   起 worker 任务 → 调 hw_pin 的协议 API → 边走边报进度/日志 → 出结论。
 *
 * 为什么要独立任务而不是在 LVGL 回调里直接干：
 *   ① 擦除/编程是毫秒~百毫秒级，卡在 LVGL 任务里会掉帧甚至喂不上看门狗；
 *   ② RDSR 轮询 busy、UART 等待应答都是**阻塞**操作；
 *   ③ 后台跑 + 前端轮询进度，是「进度条不是演出来的」的结构性保证。
 *
 * 线程模型：worker 任务持有 hw_pin；UI（LVGL）任务只通过互斥锁读快照。
 *           任何 hw_pin 调用都不得从 UI 任务发起。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROG_OP_RDID = 0,     /* READ ID : 0x9F -> JEDEC */
    PROG_OP_ERASE,        /* ERASE   : 扇区擦除 */
    PROG_OP_PROGRAM,      /* PROGRAM : 擦→页编程→校验 */
    PROG_OP_VERIFY,       /* VERIFY  : 回读逐字节比对 */
    PROG_OP_ISPSYNC,      /* ISP SYNC: UART 0x7F 握手 + 读 PID */
    PROG_OP_MAX
} prog_op_t;

typedef enum {
    PROG_ST_IDLE = 0,
    PROG_ST_RUNNING,
    PROG_ST_DONE,         /* 成功 */
    PROG_ST_FAIL          /* 真失败（含 REAL 模式没接芯片的诚实 NOFLASH） */
} prog_state_t;

#define PROG_LOG_LINES  3
#define PROG_LOG_WIDTH  40

typedef struct {
    int      state;                            /* PROG_ST_* */
    int      op;                               /* 正在跑/刚跑完的操作 */
    int      pct;                              /* 0..100 真实进度 */
    int      rc;                               /* hw_pin 返回码 (0=OK) */
    uint32_t jedec;                            /* 最近一次 RDID 原始值 */
    uint16_t pid;                              /* 最近一次 ISP GetID 的 PID（0=未读到） */
    uint8_t  ver;                              /* 最近一次 ISP GetVersion（0=未读到） */
    char     msg[16];                          /* 结论短标：OK/NOFLASH/VERIFY… */
    char     log[PROG_LOG_LINES][PROG_LOG_WIDTH];  /* 最新在最上面 */
} prog_status_t;

/** 建互斥锁 + 起 worker + 默认装 SIM（确定性模拟器） */
void prog_api_init(void);

/** 把 UI 三项设置同步到后端（档案 / 驱动 / 镜像）。
 *  worker 正在跑时拒绝（返回 <0），避免中途换档案把操作打断成半截。 */
int prog_api_cfg(int tgt_idx, int drv_idx, int img_idx);

/** 起一次操作。0 = 已受理；<0 = 上一个还在跑（不排队，直接拒） */
int prog_api_start(int op);

/** UI 自己的消息也写进同一条日志流 —— 保证屏幕上只有一个真相 */
void prog_api_log(const char *s);

/** 取状态快照（加锁拷贝，可安全从 LVGL 任务调用） */
void prog_api_status(prog_status_t *out);

const char *prog_api_image_name(int img_idx);
uint32_t    prog_api_image_size(int img_idx);

#ifdef __cplusplus
}
#endif
