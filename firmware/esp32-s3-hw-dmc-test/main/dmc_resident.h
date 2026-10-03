/**
 * dmc_resident.h — hw_dmc 常驻建链模式（上电必须通过 DMC 协议通信）
 *
 * 与 dmc_diag.c 的分工：
 *   dmc_diag    = **一次性验证**。跑完 A/B 两段就 vTaskDelete，不常驻。
 *   dmc_resident= **常驻服务**。上电起一个永不退出的任务，按 DMC 协议
 *                 反复建链 → 保活 → 掉线自动重连，直到复位为止。
 *
 * 为什么「上电必须走协议」不能靠 app_main 同步调一次：
 *   同步握手一旦对端没上电就直接失败，板子等于没有通信能力。
 *   常驻任务 = 把「能不能通信」变成**持续成立的属性**：无论对端何时
 *   上电、线何时插上，任务都会自己接上，并对外提供 dmc_resident_ready()
 *   查询；上层必须等它为真才算链路可用（不假装）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 链路参数（改这里调行为，不改逻辑）---- */
#define DMC_LINK_LOCAL_ADDR    0x01U
#define DMC_LINK_SLAVE_ADDR    0x02U
#define DMC_LINK_TIMEOUT_MS    200U     /* 单帧等待超时 */
#define DMC_LINK_HANDSHAKE_RETRY 3       /* 单轮握手重试次数 */
#define DMC_KEEPALIVE_MS        1000U    /* 保活周期 */
#define DMC_KEEPALIVE_FAIL_MAX  3        /* 连续失败几次判定掉线 */
#define DMC_STATUS_EVERY        10       /* 每 N 次保活查一次 STATUS */
#define DMC_RECONNECT_MIN_MS    200U     /* 重连退避下限 */
#define DMC_RECONNECT_MAX_MS    2000U    /* 重连退避上限 */
#define DMC_STATS_PERIOD_MS     10000U   /* 统计打印周期 */
#define DMC_HEARTBEAT_LEN       8U       /* 心跳载荷长度 */

/* ---- 对外状态 ---- */
/* 链路是否已建立（ESTABLISHED）。上层拿它当「可以发数据了」的判据。 */
bool dmc_resident_ready(void);
/* 链路状态码（hw_dmc_state_t）。非 ESTABLISHED 时给出当前所处阶段。 */
int  dmc_resident_state(void);
/* 累计统计：
 *   ok        = 保活成功次数
 *   failures  = 握手失败 + 掉线次数（首次就连不上也算，故名不叫「掉线」）
 *   reconnects= 建链成功次数（含首次）
 *   pushes    = 收到从设备主动 DATA 的次数 */
void dmc_resident_stats(uint32_t* ok, uint32_t* failures,
                        uint32_t* reconnects, uint32_t* pushes);

/* 启动常驻建链任务（幂等：重复调用只生效一次）。 */
int  dmc_resident_start(void);

#ifdef __cplusplus
}
#endif
