/**
 * dmc_resident.c — hw_dmc 常驻建链模式（ESP32-S3）
 *
 * 目标：上电之后，板子**持续**具备「按 DMC 协议通信」的能力，而不是
 *       一次性握手成功/失败就结束。
 *
 * 阶段机（每轮都从 PH_CONNECT 起，任何一步失败都退回 PH_DOWN 再退避重连）：
 *
 *      PH_WAIT_LINE ──open(uart, 无内部回环)+装 BSP──► PH_CONNECT
 *            ▲                                            │
 *            │                                      handshake_master
 *            │                                            │
 *            │                                       ┌────┴────┐
 *            │                                  OK   │         │  超时
 *            │                                       ▼         ▼
 *            │                                   PH_UP     PH_DOWN
 *            │                                       │         │
 *            │                          周期 send_data │         │ link_init(IDLE)
 *            │                          连续失败 N 次 │         │ + flush + 退避
 *            │                                       └────────►┘
 *            └──────────────────────────────────────────────┘
 *
 * ┌─ 为什么必须关掉内部回环 ─────────────────────────────────────────┐
 * │ Phase B 用 uart_set_loop_back() —— TX 在**芯片内**短接回 RX，      │
 * │ 对外引脚上一个波形都没有。它证明「字节过了真实 UART 外设」，        │
 * │ 但证明不了「上了线」。常驻模式要对外通信，必须走真实引脚           │
 * │ （IO17=TX / IO18=RX，须外接从设备 + 共地），故 loopback=false。   │
 * │ 代价：**没有对端时握手必然失败** —— 这是诚实的，不是 bug。          │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * ┌─ 为什么不假装 ───────────────────────────────────────────────────┐
 * │ 真连线模式**没有**开源自检（0xA5/0x5A 那招只在回环下有意义，     │
 * │ 在断线的真实总线上灌两个字节只是往总线塞垃圾）。所以「线通不通」   │
 * │ 的唯一判据就是握手本身：握手成功 ⇒ 线通 + 协议对 + 从设备在跑。   │
 * │ s_ready 只在 ESTABLISHED 时置真，上层拿它当通信可用性判据。        │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * ⚠️ 栈安全：链路实例、心跳缓冲全部 static（真机 IDLE 栈很小，坑 #12）。
 * ⚠️ 半双工：模块的收缓冲是 static 且非重入 ⇒ **本任务不得并发调用
 *    hw_dmc 任何函数**。要加第二条链路请另起任务之外的设计。
 */
#include "dmc_resident.h"

#include <string.h>

#include "dmc_hw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_dmc.h"
#if DMC_SLAVE_ON
#include "dmc_slave.h"
#include "dmc_cable.h"
#endif

static const char *TAG = "dmc_res";

/* ============================================================
 * 状态（全部 static，供上层查询）
 * ============================================================ */
static hw_dmc_link_t s_link;              /* 链路实例（真机栈安全） */
static volatile int    s_ready   = 0;     /* 1 = ESTABLISHED */
static volatile int    s_phase   = 0;
static volatile uint32_t s_ok        = 0; /* 成功保活次数 */
static volatile uint32_t s_failures = 0; /* 握手失败 + 掉线（故不叫 drops） */
static volatile uint32_t s_reconnects= 0; /* 重连成功次数 */
static volatile uint32_t s_pushes    = 0; /* 收到从设备主动 DATA 的次数 */
static uint32_t s_tx = 0, s_rx = 0, s_crc = 0, s_to = 0; /* 跨重连累计 */
static uint8_t s_heartbeat[DMC_HEARTBEAT_LEN];
static uint16_t s_hb_ctr = 0;
static int  s_started = 0;

enum { PH_WAIT_LINE = 0, PH_CONNECT, PH_UP, PH_DOWN };

static const char* ph_name(int p)
{
    switch (p) {
    case PH_WAIT_LINE: return "WAIT_LINE";
    case PH_CONNECT:   return "CONNECT";
    case PH_UP:        return "UP";
    case PH_DOWN:      return "DOWN";
    default:           return "?";
    }
}

/* 心跳载荷：'D''M''C''H''B''B' + 计数低/高 + 校验异或。
 * 之所以带内容而不发 0 字节：0 字节载荷在协议上合法，但那只能证
 * 「帧发出去了」，证不了「业务数据原样编进了帧」。带魔数后，
 * 抓包/对端日志能直接对上这 8 个字节。 */
static void hb_build(uint8_t* p, uint16_t ctr)
{
    p[0] = 'D'; p[1] = 'M'; p[2] = 'C'; p[3] = 'H';
    p[4] = 'B'; p[5] = 'B';
    p[6] = (uint8_t)(ctr & 0xFFU);
    p[7] = (uint8_t)((ctr >> 8) & 0xFFU);
}

/* 退避递增并**硬性封顶**。
 * 🕳️ 2026-10-02 真机日志逮到的真 bug: 原写法
 *     vTaskDelay(backoff);  if (backoff < MAX) backoff *= 2;
 *   判断在**延迟之后**做, 于是 1600 < 2000 成立 → 翻成 3200 才停,
 *   实测日志连续打出「退避 3200ms」而 DMC_RECONNECT_MAX_MS=2000
 *   ⇒ 声明的上限根本没兑现(实际封顶 3200 = 2×MAX)。
 *   修法 = 封顶放在翻倍之前, 保证任何路径都不可能超过 MAX。 */
static void backoff_grow(uint32_t* d)
{
    if (*d >= DMC_RECONNECT_MAX_MS) { *d = DMC_RECONNECT_MAX_MS; return; }
    *d *= 2;
    if (*d > DMC_RECONNECT_MAX_MS) *d = DMC_RECONNECT_MAX_MS;
}

/* 链路计数器在 hw_dmc_link_init() 里被 memset 清零, 而常驻模式每轮
 * 重连都要 init ⇒ 直接读永远读到 0(实测 tx=0 rx=0, 统计形同虚设)。
 * 故在**清零之前**把上一段的计数收割到累计值里。 */
static void harvest_stats(void)
{
    uint32_t tx = 0, rx = 0, crc = 0, to = 0;
    hw_dmc_stats(&s_link, &tx, &rx, &crc, &to);
    s_tx += tx; s_rx += rx; s_crc += crc; s_to += to;
}

bool dmc_resident_ready(void) { return s_ready ? true : false; }
int  dmc_resident_state(void){ return hw_dmc_link_state(&s_link); }
void dmc_resident_stats(uint32_t* ok, uint32_t* failures,
                        uint32_t* reconnects, uint32_t* pushes)
{
    if (ok)         *ok         = s_ok;
    if (failures)   *failures   = s_failures;
    if (reconnects) *reconnects = s_reconnects;
    if (pushes)     *pushes     = s_pushes;
}

/* ============================================================
 * 主任务
 * ============================================================ */
static void resident_task(void *arg)
{
    uint32_t backoff = DMC_RECONNECT_MIN_MS;
    uint32_t next_keepalive = 0;
    uint32_t next_stats     = 0;
    int      fail_run = 0;
    int      up_cycle = 0;
    int      r;

    (void)arg;

    ESP_LOGI(TAG, "==== hw_dmc 常驻建链启动 (上电必须走 DMC 协议) ====");
    ESP_LOGI(TAG, "地址 local=0x%02X slave=0x%02X 超时=%ums 保活=%ums 掉线阈值=%d",
             DMC_LINK_LOCAL_ADDR, DMC_LINK_SLAVE_ADDR,
             DMC_LINK_TIMEOUT_MS, DMC_KEEPALIVE_MS, DMC_KEEPALIVE_FAIL_MAX);
    ESP_LOGI(TAG, "⚠ 常驻模式 loopback=OFF：需要从设备 + 跳线 IO%d(TX)→从RX / 从TX→IO%d(RX) + 共地",
             DMC_UART_TX, DMC_UART_RX);

    /* 上电复位（不清 BSP，坑 #25）。先 init 再装 BSP，顺序上更无歧义。 */
    hw_dmc_init(NULL);

    s_phase = PH_WAIT_LINE;
    next_stats = dmc_hw_now_ms();

    for (;;) {
        uint32_t now = dmc_hw_now_ms();

        switch (s_phase) {

        /* ---------- 等线：开 UART（无回环）+ 装真机 BSP ---------- */
        case PH_WAIT_LINE:
            if (dmc_hw_install_ex(true, false) != 0) {
                ESP_LOGW(TAG, "[line] UART%d 打不开，退避 %ums 后重试",
                         DMC_UART_PORT, (unsigned)backoff);
                vTaskDelay(pdMS_TO_TICKS(backoff));
                backoff_grow(&backoff);
                break;
            }
            dmc_hw_dump();
#if DMC_SLAVE_ON && DMC_SLAVE_LINK == 0
            /* ★ 必须放在**这里**：本函数内部刚跑完主机的 uart_set_pin(),
             *   它把 IO18 设成了 INPUT（顺手关掉了从机 UART2 的 TX 驱动）。
             *   放到别处都会被随后这次调用覆盖回去 —— 详见 dmc_slave.h。 */
            dmc_bus_pads_bidir();
#endif
            backoff = DMC_RECONNECT_MIN_MS;
            s_phase = PH_CONNECT;
            ESP_LOGI(TAG, "[line] UART 就绪，进入 CONNECT");
            break;

        /* ---------- 建链：HELLO → HELLO_ACK ---------- */
        case PH_CONNECT:
            harvest_stats();               /* 先收割再清零, 否则统计永远是 0 */
            hw_dmc_link_init(&s_link, DMC_LINK_LOCAL_ADDR,
                             DMC_LINK_SLAVE_ADDR, DMC_LINK_TIMEOUT_MS);
            s_ready = 0;
            fail_run = 0;

            r = hw_dmc_handshake_master(&s_link, DMC_LINK_HANDSHAKE_RETRY);
            if (r == HW_DMC_ERR_OK) {
                s_ready = 1;
                s_reconnects++;
                /* 🕳️ 建链成功 ⇒ 退避归零。原代码只在等线成功时复位, 于是
                 *   「连上→用一阵→掉线」时重连仍要等封顶的 3.2 秒才重试,
                 *   对刚恢复的链路是白白浪费。成功即证明对端在, 该从快开始。 */
                backoff = DMC_RECONNECT_MIN_MS;
                s_phase = PH_UP;
                up_cycle = 0;
                next_keepalive = dmc_hw_now_ms() + DMC_KEEPALIVE_MS;
                ESP_LOGI(TAG, "[link] ✔ 建链成功 ESTABLISHED seq_tx=%u seq_rx=%u",
                         (unsigned)s_link.seq_tx, (unsigned)s_link.seq_rx);
            } else {
                s_failures++;
                s_phase = PH_DOWN;
                ESP_LOGW(TAG, "[link] ✘ 握手失败 rc=%d(%s) —— 无应答即：对端未上电/线未接/波特率不符",
                         r, hw_dmc_err_name(r));
            }
            break;

        /* ---------- 在线：周期保活 ---------- */
        case PH_UP:
            if (now < next_keepalive) { vTaskDelay(pdMS_TO_TICKS(20)); break; }
            next_keepalive = now + DMC_KEEPALIVE_MS;

            hb_build(s_heartbeat, s_hb_ctr);
            r = hw_dmc_send_data(&s_link, s_heartbeat, DMC_HEARTBEAT_LEN, 2);

            if (r == HW_DMC_ERR_OK) {
                s_ok++;
                s_hb_ctr++;
                fail_run = 0;
                up_cycle++;

                /* 周期性查一次 STATUS：走另一条命令，证明不是只有 DATA 通 */
                if (DMC_STATUS_EVERY > 0 && (up_cycle % DMC_STATUS_EVERY) == 0) {
                    uint8_t st = 0xFF;
                    int rs = hw_dmc_query_status(&s_link, &st, DMC_LINK_TIMEOUT_MS);
                    ESP_LOGI(TAG, "[alive] ctr=%u  status rc=%d byte=0x%02X",
                             (unsigned)(s_hb_ctr - 1), rs, st);
                }

                /* 顺手探一次从设备是否主动推 DATA（短超时，非阻塞心跳） */
                if ((up_cycle % 4) == 0) {
                    static uint8_t push[64];
                    uint16_t plen = 0;
                    int rp = hw_dmc_recv_data(&s_link, push, (int)sizeof(push),
                                              &plen, 50);
                    if (rp == HW_DMC_ERR_OK) {
                        s_pushes++;
                        ESP_LOGI(TAG, "[push] 从设备主动 DATA %u 字节: %c%c%c%c",
                                 (unsigned)plen,
                                 (plen > 0) ? push[0] : '?',
                                 (plen > 1) ? push[1] : '?',
                                 (plen > 2) ? push[2] : '?',
                                 (plen > 3) ? push[3] : '?');
                    }
                    /* 超时是正常情形（从设备没事要报），不计入失败 */
                }
            } else {
                fail_run++;
                ESP_LOGW(TAG, "[alive] ✘ 保活失败 rc=%d(%s) 连续 %d/%d",
                         r, hw_dmc_err_name(r), fail_run, DMC_KEEPALIVE_FAIL_MAX);
                if (fail_run >= DMC_KEEPALIVE_FAIL_MAX) {
                    s_failures++;
                    s_ready = 0;
                    s_phase = PH_DOWN;
                    ESP_LOGE(TAG, "[link] ✘ 判定掉线，退回重连");
                }
            }
            break;

        /* ---------- 掉线：复位链路 + 清总线残留 + 退避 ---------- */
        case PH_DOWN:
            s_ready = 0;
            harvest_stats();               /* 同上: 清零前先收割 */
            hw_dmc_link_init(&s_link, DMC_LINK_LOCAL_ADDR,
                             DMC_LINK_SLAVE_ADDR, DMC_LINK_TIMEOUT_MS);
            /* ⚠️ 必须清接收缓冲：上一帧的残留字节会让下一次握手把
             *    垃圾当成帧头右移重同步，白白烧掉一轮超时。 */
            dmc_hw_uart_flush();
            ESP_LOGW(TAG, "[link] 退避 %ums 后重连 (累计 ok=%u fail=%u reconnect=%u)",
                     (unsigned)backoff, (unsigned)s_ok,
                     (unsigned)s_failures, (unsigned)s_reconnects);
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff_grow(&backoff);        /* 硬封顶, 不可能超过 MAX */
            s_phase = PH_CONNECT;
            break;

        default:
            s_phase = PH_WAIT_LINE;
            break;
        }

        /* ---------- 周期统计（不刷屏，只在整秒附近打一行）---------- */
        now = dmc_hw_now_ms();
        if (now >= next_stats) {
            uint32_t tx = 0, rx = 0, crc = 0, to = 0;
            /* 本段增量 + 已收割的累计 = 跨重连的总量 */
            hw_dmc_stats(&s_link, &tx, &rx, &crc, &to);
            tx += s_tx; rx += s_rx; crc += s_crc; to += s_to;
            ESP_LOGI(TAG, "[stat] phase=%s ready=%d state=%s ok=%u fail=%u reconnect=%u push=%u tx=%u rx=%u crc_err=%u to_err=%u",
                     ph_name(s_phase), s_ready,
                     hw_dmc_state_name(hw_dmc_link_state(&s_link)),
                     (unsigned)s_ok, (unsigned)s_failures, (unsigned)s_reconnects,
                     (unsigned)s_pushes,
                     (unsigned)tx, (unsigned)rx, (unsigned)crc, (unsigned)to);
#if DMC_SLAVE_ON
            /* 「双口供词」：从机一侧**独立**计数。主机说「我收到 N 帧应答」，
             * 从机说「我发了 N 帧」—— 两边对上，才排除了「日志打的是自己
             * 循环的产物」这类自证。若 slave.rx=0 而 master.rx>0，
             * 说明看到的不是真实链路。 */
            {
                uint32_t srx = 0, stx = 0, scrc = 0, spush = 0;
                dmc_slave_stats(&srx, &stx, &scrc, &spush);
                ESP_LOGI(TAG, "[slave] mode=%s rx=%u tx=%u crc_err=%u push=%u  (与上行对照)",
                         DMC_SLAVE_MODE_NAME,
                         (unsigned)srx, (unsigned)stx, (unsigned)scrc, (unsigned)spush);
            }
#endif
            next_stats = now + DMC_STATS_PERIOD_MS;
        }

        /* 让出 CPU：FreeRTOS 任务不主动让会让 IDLE 饿死（坑 #6） */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int dmc_resident_start(void)
{
    if (s_started) return 0;
    s_started = 1;
    /* 栈 8192：模块内部有若干 static 缓冲，但状态机 + 诊断格式化仍需余量 */
    if (xTaskCreate(resident_task, "dmc_res", 8192, NULL, 5, NULL) != pdPASS)
        return -1;
    return 0;
}
