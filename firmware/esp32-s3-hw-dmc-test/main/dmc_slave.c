/**
 * dmc_slave.c — 板载 DMC 从设备仿真器（ESP32-S3，第二组 UART）
 *
 * 职责：在真实外设链路上应答主机的 4 类命令，让常驻模式的 PH_UP 跑起来。
 * 线格式 / CRC 全部调用 hw_dmc 的纯函数，**不在本文件手抄任何协议细节**
 * （单一真相源：手抄副本必然与模块漂移，见 hw_dmc.c dmc_state_join_fnv 的教训）。
 *
 * ⚠️ 栈安全：收缓冲与成帧缓冲全 static（真机 IDLE 栈很小，家族坑 #12）。
 * ⚠️ 只用模块的 pack/unpack/crc16，**不碰** hw_dmc 的链路 API ——
 *    BSP 是全局单例，主从同时驱动会互相踩收发缓冲。
 */
#include "dmc_slave.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "dmc_hw.h"     /* 主机侧 UART 口/脚定义 + dmc_hw_install_ex（判别探针用） */
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_dmc.h"
#include "soc/gpio_struct.h"   /* GPIO.enable1 / GPIO.func_out_sel_cfg —— 判别 matrix 出向 */
#include "soc/io_mux_reg.h"     /* IO_MUX_GPIOx_REG / MCU_SEL                  */

static const char *TAG = "dmc_slave";

/* ============================================================
 * 状态（全 static）
 * ============================================================ */
static uint8_t  s_rxbuf[HW_DMC_MAX_FRAME];   /* 逐字节累积的接收缓冲 */
static uint8_t  s_txbuf[HW_DMC_MAX_FRAME];   /* 成帧缓冲           */
static uint8_t  s_plbuf[HW_DMC_MAX_PAYLOAD]; /* 载荷暂存           */
static int      s_started = 0;
static uint16_t s_seq_tx = 0;                 /* 主机 DATA 的 seq 回声 */
static uint16_t s_seq_push = 0;               /* 从机主动 DATA 的 seq  */
static uint32_t s_rx_frames = 0, s_tx_frames = 0, s_err_crc = 0, s_push = 0;
static uint32_t s_next_push = 0;
static uint32_t s_next_stat = 0;
static int      s_opened = 0;

/* 应答是否故意破坏 CRC（DMC_SLAVE_MODE=2）。
 * 用一个开关而不是复制一整套 s_handle —— 复制出来的第二条应答路径必然
 * 与第一条漂移，而「正常臂与注入臂走**同一段**映射代码」正是这个实验
 * 成立的前提：两臂唯一的差别只能是 CRC 那一位。 */
#if DMC_SLAVE_MODE == 2
static const int s_bad_crc = 1;
#else
static const int s_bad_crc = 0;
#endif

#define SLAVE_PUSH_MS   3000U   /* 主动推 DATA 周期 */
#define SLAVE_STAT_MS   10000U  /* 统计打印周期     */
#define SLAVE_IDLE_MS   20U     /* 无字节时的轮询休眠 */

/* ============================================================
 * 真实 UART2 收发
 * ============================================================ */
static int s_tx_one(uint8_t ch)
{
    int w = uart_write_bytes(DMC_SLAVE_PORT, (const char*)&ch, 1);
    return (w == 1) ? 0 : -1;
}

static int s_rx_one(uint32_t timeout_ms)
{
    uint8_t b;
    int r = uart_read_bytes(DMC_SLAVE_PORT, &b, 1, pdMS_TO_TICKS(timeout_ms));
    return (r == 1) ? (int)b : -1;
}

/* 发一整帧。bad_crc=true 时把 CRC 末字节取反 —— 完整性故障注入：
 * 主机必须报 ERR_CRC 而不是「假装收到」。
 *
 * ⚠️ 静默臂（DMC_SLAVE_MODE=1）**在这里**早退，而不是在 s_reply 里早退。
 *    放错位置的代价是实测出来的：s_reply 先 return ⇒ s_send_frame 与
 *    s_bad_crc 在该臂下都成了「定义但没用到」的静态符号 ⇒ -Werror 直接
 *    编译失败（第一版 MODE=1 就是这么挂的）。所有臂必须走**同一条调用链**，
 *    否则「三臂只差一个变量」这个前提从代码结构上就不成立。 */
static int s_send_frame(uint8_t cmd, const uint8_t* pl, uint16_t plen, bool bad_crc)
{
#if DMC_SLAVE_MODE == 1
    (void)cmd; (void)pl; (void)plen; (void)bad_crc;
    /* 静默臂下本函数**永不发送** ⇒ s_tx_one / s_txbuf 成为死符号。
     * 显式引用一下：既消掉 -Wunused 警告，也把「这一臂就是不发」这件事
     * 写在代码里，而不是留在读代码的人心里。 */
    (void)s_tx_one; (void)s_txbuf;
    return -1;                        /* 阴性对照：一个字节都不发 */
#else
    int n, i;
    n = hw_dmc_pack(cmd, pl, plen, s_txbuf, (int)sizeof(s_txbuf));
    if (n < 0) return n;
    if (bad_crc && n > 0) s_txbuf[n - 1] = (uint8_t)(s_txbuf[n - 1] ^ 0xFFU);
    for (i = 0; i < n; i++) {
        if (s_tx_one(s_txbuf[i]) < 0) return -1;
    }
    s_tx_frames++;
    return n;
#endif
}

/* ============================================================
 * 命令应答
 * ============================================================ */
static void s_reply(uint8_t cmd, const uint8_t* pl, uint16_t plen)
{
    (void)s_send_frame(cmd, pl, plen, s_bad_crc);
}

static void s_handle(uint8_t cmd, const uint8_t* pl, uint16_t plen)
{
    switch (cmd) {
    case HW_DMC_CMD_HELLO:
        /* 与模块 hw_dmc_handshake_slave 同一套 8 字节应答（复制协议语义，不复制代码）*/
        s_plbuf[0] = DMC_SLAVE_ADDR; s_plbuf[1] = 0x01;
        s_plbuf[2] = 0xFF;           s_plbuf[3] = 0xFF;
        s_plbuf[4] = 0; s_plbuf[5] = 0; s_plbuf[6] = 0; s_plbuf[7] = 0;
        s_reply(HW_DMC_CMD_HELLO_ACK, s_plbuf, 8);
        break;

    case HW_DMC_CMD_DATA:
        /* 必须回**主机给的 seq**：hw_dmc_send_data 逐字节比对 ack[0..1] */
        if (plen >= 2) { s_seq_tx = (uint16_t)(pl[0] | ((uint16_t)pl[1] << 8)); }
        s_plbuf[0] = (uint8_t)(s_seq_tx & 0xFFU);
        s_plbuf[1] = (uint8_t)((s_seq_tx >> 8) & 0xFFU);
        s_reply(HW_DMC_CMD_DATA_ACK, s_plbuf, 2);
        break;

    case HW_DMC_CMD_STATUS:
        /* 低 4 位放已收帧数（低 4 bit），让主机侧看到该字节**在变** = 对端活着 */
        s_plbuf[0] = (uint8_t)(0xA0U | ((s_rx_frames + 1U) & 0x0FU));
        s_reply(HW_DMC_CMD_STATUS_RSP, s_plbuf, 1);
        break;

    case HW_DMC_CMD_RESET:
        s_seq_tx = 0; s_seq_push = 0;
        s_reply(HW_DMC_CMD_RESET_ACK, NULL, 0);
        break;

    default:
        s_reply(HW_DMC_CMD_NACK, NULL, 0);
        break;
    }
}

/* 从机主动推一帧 DATA（证「双工反向方向」也通，而不只是被动应答）。
 * 载荷 'D''M''C''P' + 计数 = 与主机心跳 'D''M''C''H''B''B' 区分得开，
 * 主机日志里 [push] 打的正是这 4 个字节。 */
static void s_do_push(void)
{
    uint8_t pl[2 + 6];
    int n;

    pl[0] = (uint8_t)(s_seq_push & 0xFFU);
    pl[1] = (uint8_t)((s_seq_push >> 8) & 0xFFU);
    pl[2] = 'D'; pl[3] = 'M'; pl[4] = 'C'; pl[5] = 'P';
    pl[6] = (uint8_t)(s_seq_push & 0xFFU);
    pl[7] = (uint8_t)((s_seq_push >> 8) & 0xFFU);

    /* bad_crc 显式传 false：完整性注入**只**针对应答，推帧必须是好帧。
     * 否则主机在等应答时收到坏推帧，会把「应答 CRC 坏」和「推帧 CRC 坏」
     * 两种原因混在一起 —— 结论就变含糊了。走同一个 s_send_frame 而不是
     * 另写一遍写循环，也顺带消掉了「两份成帧代码必然漂移」的老毛病。 */
    n = s_send_frame(HW_DMC_CMD_DATA, pl, 8, false);
    if (n < 0) return;                /* 静默臂：一帧都没发，push 不计数 */
    s_push++;
    s_seq_push++;
}

/* ============================================================
 * 从机主循环
 * ============================================================ */
static void slave_task(void *arg)
{
    int n = 0;
    (void)arg;

    ESP_LOGI(TAG, "==== 板载从设备仿真器启动 ====");
    ESP_LOGI(TAG, "UART%d TX=IO%d RX=IO%d baud=%d addr=0x%02X 接线=%s",
             DMC_SLAVE_PORT, DMC_SLAVE_TX, DMC_SLAVE_RX, DMC_SLAVE_BAUD,
             DMC_SLAVE_ADDR, DMC_SLAVE_WIRE);
    ESP_LOGI(TAG, "应答模式=%s", DMC_SLAVE_MODE_NAME);
    ESP_LOGW(TAG, "⚠ 这是**板载仿真器**（同一颗芯片的第二个 UART），"
                  "不是外部真实从设备；外部波形需 DMC_SLAVE_LINK=1 + 外部跳线/示波器");

    s_next_push = (uint32_t)(esp_timer_get_time() / 1000) + SLAVE_PUSH_MS;
    s_next_stat = (uint32_t)(esp_timer_get_time() / 1000) + SLAVE_STAT_MS;

    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int r;

        /* 1) 抽一个字节 */
        r = s_rx_one(SLAVE_IDLE_MS);
        if (r >= 0) {
            if (n < (int)sizeof(s_rxbuf)) s_rxbuf[n++] = (uint8_t)r;
            else n = 0;                       /* 缓冲满 = 流被打乱，丢弃重来（诚实重同步） */
        }

        /* 2) 尝试解帧 */
        if (n >= (int)HW_DMC_MIN_FRAME) {
            uint8_t cmd = 0;
            const uint8_t* pl = NULL;
            uint16_t plen = 0;
            int declared;

            if (s_rxbuf[0] != (uint8_t)HW_DMC_SYNC_0 ||
                s_rxbuf[1] != (uint8_t)HW_DMC_SYNC_1) {
                /* 同步头失配 → 右移 1 字节重同步（逐字节流残留，与主机同款策略） */
                int i;
                for (i = 0; i + 1 < n; i++) s_rxbuf[i] = s_rxbuf[i + 1];
                n--;
            } else {
                declared = (int)s_rxbuf[2];
                if (declared >= (int)HW_DMC_MIN_FRAME &&
                    declared <= (int)HW_DMC_MAX_FRAME && n >= declared) {
                    int u = hw_dmc_unpack(s_rxbuf, n, &cmd, &pl, &plen);
                    if (u == HW_DMC_ERR_OK) {
                        s_rx_frames++;
                        if (plen > 0 && plen <= (uint16_t)sizeof(s_plbuf))
                            memcpy(s_plbuf, pl, plen);   /* 载荷转存：pl 指向 s_rxbuf */
                        s_handle(cmd, s_plbuf, plen);
                        n = 0;
                    } else if (u == HW_DMC_ERR_CRC) {
                        s_err_crc++;
                        ESP_LOGW(TAG, "收到坏 CRC 帧，丢弃");
                        n = 0;
                    } else {
                        n = 0;
                    }
                }
            }
        }

        /* 3) 主动推帧 */
        if (now >= s_next_push) {
            s_do_push();
            s_next_push = now + SLAVE_PUSH_MS;
        }

        /* 4) 统计 */
        if (now >= s_next_stat) {
            ESP_LOGI(TAG, "[slave-stat] rx=%u tx=%u crc_err=%u push=%u mode=%d",
                     (unsigned)s_rx_frames, (unsigned)s_tx_frames,
                     (unsigned)s_err_crc, (unsigned)s_push, DMC_SLAVE_MODE);
            s_next_stat = now + SLAVE_STAT_MS;
        }
    }
}

/* ============================================================
 * 双向 pad 修复（详见 dmc_slave.h 里的完整根因）
 * ============================================================ */
#if DMC_SLAVE_LINK == 0
void dmc_bus_pads_bidir(void)
{
    /* 收尾修复：把两个 pad 的输入+输出驱动都打开。
     *
     * 为什么必须放最后：uart_set_pin() 的 RX 分支会
     *   gpio_set_direction(rx_io, GPIO_MODE_INPUT) —— 顺手**关掉**该 pad
     * 的输出驱动。两个 UART 各设一次自己的 RX，最后那次会把先设的那个
     * 覆盖回去 ⇒ 谁先谁后都有一边被关掉，**顺序无关地错**。
     *   （实测：从机后启动 → 主机 uart_set_pin 把 IO8 设成 INPUT，
     *     从机 UART2 的 TX 驱动就死了。）
     *
     * 为什么不能用 gpio_config()：它结尾有
     *   gpio_hal_iomux_func_sel(io_reg, PIN_FUNC_GPIO)   // "By default,
     *   all the pins have to be configured as GPIO pins"
     * 会把 IOMUX 路由切回普通 GPIO，外设 TX 路由一并被顶掉
     * ⇒ 修复①就是这么把从机 rx 从 66 打到 0 的，**已被实验证伪**。
     *
     * 为什么 gpio_set_direction() 安全：读过实现，它**只碰 enable 位**
     * （gpio_input_enable/output_enable），不动 PIN_FUNC_*、不动 matrix
     * 路由；而 matrix 路的 TX 恰恰靠 enable 位生效。
     *   ⚠️ 必须用 INPUT_OUTPUT 而不是 OUTPUT：后者会调
     *      gpio_input_disable()，而对端 UART 的 RX 正是靠 matrix in
     *      采这个 pad 的输入缓冲器。
     */
    if (gpio_set_direction(DMC_SLAVE_RX, GPIO_MODE_INPUT_OUTPUT) != ESP_OK)
        ESP_LOGE(TAG, "IO%d 双向使能失败（主机 UART1 TX 驱动）", DMC_SLAVE_RX);
    if (gpio_set_direction(DMC_SLAVE_TX, GPIO_MODE_INPUT_OUTPUT) != ESP_OK)
        ESP_LOGE(TAG, "IO%d 双向使能失败（从机 UART2 TX 驱动）", DMC_SLAVE_TX);

    ESP_LOGI(TAG, "IO%d(主机TX)/IO%d(从机TX) 双向使能已补回 —— "
                  "两个 uart_set_pin 都跑完后的收尾动作，顺序无关",
             DMC_SLAVE_RX, DMC_SLAVE_TX);
}
#endif

/* ============================================================
 * 打开 / 任务
 * ============================================================ */
static int s_open_uart(void)
{
    uart_config_t cfg = {
        .baud_rate  = DMC_SLAVE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (s_opened) return 0;
    if (uart_driver_install(DMC_SLAVE_PORT, 1024, 1024, 0, NULL, 0) != ESP_OK) return -1;
    if (uart_param_config(DMC_SLAVE_PORT, &cfg) != ESP_OK) return -1;
    if (uart_set_pin(DMC_SLAVE_PORT, DMC_SLAVE_TX, DMC_SLAVE_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;
    uart_set_loop_back(DMC_SLAVE_PORT, false);
    uart_flush_input(DMC_SLAVE_PORT);
    s_opened = 1;
    return 0;
}

int dmc_slave_open_only(void)
{
    return s_open_uart();
}

int dmc_slave_start(void)
{
    if (s_started) return 0;
    s_started = 1;

    if (s_open_uart() != 0) return -1;

    /* 优先级高于主机(5)：主机阻塞等应答时必须立刻被唤醒服务 */
    if (xTaskCreate(slave_task, "dmc_slave", 4096, NULL, 6, NULL) != pdPASS) return -1;
    return 0;
}

void dmc_slave_stats(uint32_t* rx_frames, uint32_t* tx_frames,
                     uint32_t* err_crc, uint32_t* push)
{
    if (rx_frames) *rx_frames = s_rx_frames;
    if (tx_frames) *tx_frames = s_tx_frames;
    if (err_crc)   *err_crc   = s_err_crc;
    if (push)      *push      = s_push;
}

#if DMC_SLAVE_LINK == 0
/* ---- pad 寄存器 dump：三分判读的唯一依据 ----
 * C0 已证明「gpio_get_level 能看到外设驱动的 pad」，所以 IO8/IO9 的
 * 0/7000 是**真的没驱动**。剩下的问题只能在寄存器里找。读三样：
 *
 *   iomux_func_sel  = IO_MUX_GPIOx_REG[12:14] (MCU_SEL)
 *       0 = 该 pad 归 GPIO 矩阵管（matrix 路的必要条件）
 *   func_out_sel_cfg= GPIO.func_out_sel_cfg[io].val
 *       ROM 的 gpio_matrix_out 写的就是它：应等于 UARTxTXD_OUT_IDX
 *       （U1TXD=15 / U2TXD=18）。若仍为 0 ⇒ ROM 根本没写这根线。
 *   oe              = GPIO.enable1 bit io（pad 输出驱动使能）
 *
 * ⚠️ 注意 S3 的 func_out_sel_cfg 是 9 bit，与原版 ESP32 的
 *    out_sig_map[].output_sel(8bit)+enable(1bit) **不同构** ——
 *    所以「func_out_sel_cfg != 0」与「out_sig_map.enable == 1」
 *    不是同一件事，不能拿原版的经验直接套。
 */
static uint32_t s_iomux_reg(int io)
{
    switch (io) {
    case  8: return IO_MUX_GPIO8_REG;
    case  9: return IO_MUX_GPIO9_REG;
    case 10: return IO_MUX_GPIO10_REG;
    case 11: return IO_MUX_GPIO11_REG;
    case 17: return IO_MUX_GPIO17_REG;
    case 18: return IO_MUX_GPIO18_REG;
    default: return 0;
    }
}

static void s_dump_pad(const char *stage, int io)
{
    uint32_t fn   = (s_iomux_reg(io) >> MCU_SEL_S) & MCU_SEL_V;
    uint32_t ocfg = GPIO.func_out_sel_cfg[io].val;
    uint32_t en   = (GPIO.enable1.val >> io) & 1u;
    ESP_LOGW(TAG, "[%-8s] IO%-2d iomux_func_sel=%u func_out_sel_cfg=0x%03x oe=%u",
             stage, io, (unsigned)fn, (unsigned)ocfg, (unsigned)en);
}

/* ============================================================
 * s_count_low / s_burst_and_count —— 判别探针的采样基元
 * ============================================================ */
/* 发送一段固定图案的长突发，同时连续采样 pad，返回低电平点数。
 *
 * 为什么必须「长突发」：115200 下 1 bit ≈ 86.8us，4 字节只有 ~347us，
 * 而 esp_rom_delay_us(1) 的实际耗时在 flash 缓存命中与否时差好几倍
 * ⇒ 短突发很容易整个错过采样窗口，误报「没驱动」。
 * 0x55 = 01010101，8N1 下每字节 = 起始(低) + 4 低 + 4 高 + 停止(高)
 * ⇒ 低电平占比恒为 50%。64 字节 = 640 bit ≈ 5.6ms，窗口取 7000 步足够覆盖。
 */
/* 只采样，不发送（给纯 GPIO 对照用）。 */
static int s_sample(int pin, int span_us)
{
    int low = 0, i;
    for (i = 0; i < span_us; i++) {
        if (gpio_get_level((gpio_num_t)pin) == 0) low++;
        esp_rom_delay_us(1);
    }
    return low;
}

static int s_burst_and_count(int port, int pin, int span_us)
{
    uint8_t pat[64];
    int i;
    for (i = 0; i < 64; i++) pat[i] = 0x55;

    uart_flush_input(port);
    uart_write_bytes(port, (const char *)pat, 64);
    i = s_sample(pin, span_us);
    uart_wait_tx_done(port, pdMS_TO_TICKS(300));
    return i;
}

int dmc_wire_probe(void)
{
    static uint8_t buf[80];
    int got, ok = 0, low;

    ESP_LOGW(TAG, "==== 判别探针（从机任务尚未启动，无抢字节竞争）====");
    ESP_LOGW(TAG, "接线: 主机UART%d TX=IO%d RX=IO%d | 从机UART%d TX=IO%d RX=IO%d",
             DMC_UART_PORT, DMC_UART_TX, DMC_UART_RX,
             DMC_SLAVE_PORT, DMC_SLAVE_TX, DMC_SLAVE_RX);

    if (dmc_hw_install_ex(true, false) != 0) {
        ESP_LOGE(TAG, "探针: 主机 UART 打不开");
        return -1;
    }
    if (dmc_slave_open_only() != 0) {
        ESP_LOGE(TAG, "探针: 从机 UART 打不开");
        return -1;
    }
    dmc_bus_pads_bidir();
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGW(TAG, "---- pad 寄存器 dump: 刚补完双向使能（未发送）----");
    s_dump_pad("pre", DMC_SLAVE_TX);
    s_dump_pad("pre", DMC_SLAVE_RX);

    /* ---------- C0 已知良品对照（**决定性**） ----------
     * 目的：验证「gpio_get_level 能不能看到**外设驱动**的 pad」。
     *
     * 之前那个阳性对照只验了「纯 GPIO 驱动」的 pad，而本探针要测的是
     * 「UART 外设经 IOMUX / matrix 驱动」的 pad —— 这是**两种不同的驱动源**。
     * 若 gpio_get_level 读的是 pad 的输入缓冲器，它对两者都应可见；
     * 但若它在 S3 上读的是某个只反映 GPIO 输出寄存器/输入选择器的节点，
     * 那它对「外设驱动的 pad」**一律读回空闲高电平** ——
     * 那么 A/B 的 0/400 就**不是**「没驱动」，而是采样方法的盲点，
     * 前面一整轮「pad 使能/路由层坏了」的结论全部作废。
     *
     * C0 用 UART1 的**原生 IOMUX 脚** IO17/IO18（uart_pins.h 里
     * U1TXD_GPIO_NUM=17），这是一条**已知一定通**的路径，
     * 因此它是判定「采样方法有没有盲点」的金标准。
     */
    ESP_LOGW(TAG, "---- C0 已知良品对照: UART1 → 原生 IOMUX 脚 IO17 ----");
    uart_set_pin(DMC_UART_PORT, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    vTaskDelay(pdMS_TO_TICKS(20));
    low = s_burst_and_count(DMC_UART_PORT, 17, 7000);
    ESP_LOGW(TAG, "C0 采样 IO17(IOMUX 外设驱动) 低 %d/7000 ⇒ %s", low,
             low > 500
             ? "采样通路**能**看到外设驱动的 pad ⇒ A/B 的 0/400 是真的没驱动"
             : "⚠️ gpio_get_level **看不到**外设驱动的 pad ⇒ A/B 的 0/400 是采样盲点，证伪不了任何事");
    uart_flush_input(DMC_UART_PORT);
    /* 复原到 matrix 脚 */
    uart_set_pin(DMC_UART_PORT, DMC_UART_TX, DMC_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    dmc_bus_pads_bidir();
    vTaskDelay(pdMS_TO_TICKS(20));

    /* C0 的 IO17 是**已知良品**（IOMUX 驱动、采样见 1973/7000）。
     * 把它和 IO8/IO9 的 dump 摆在一起对照，就能一眼看出
     * matrix 路比 IOMUX 路少了哪一环。 */
    ESP_LOGW(TAG, "---- pad 寄存器 dump: C0 之后（IO17=已知良品，IO8/9=待判）----");
    s_dump_pad("good", 17);
    s_dump_pad("post", DMC_SLAVE_TX);
    s_dump_pad("post", DMC_SLAVE_RX);

    ESP_LOGW(TAG, "空闲电平: IO%d=%d IO%d=%d（期望都是 1）",
             DMC_SLAVE_TX, gpio_get_level((gpio_num_t)DMC_SLAVE_TX),
             DMC_SLAVE_RX, gpio_get_level((gpio_num_t)DMC_SLAVE_RX));

    /* A) 从机 → 主机：UART2 写，UART1 读（验 IO8 这条 pad） */
    low = s_burst_and_count(DMC_SLAVE_PORT, DMC_SLAVE_TX, 7000);
    ESP_LOGW(TAG, "A) 发送中采样 IO%d 低 %d/7000 → %s", DMC_SLAVE_TX, low,
             low > 500 ? "pad 确实在驱动，问题在**对端采不到**"
                       : "pad 发送期间无低电平");
    got = uart_read_bytes(DMC_UART_PORT, buf, 64, pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "A) 从机→主机 (IO%d): %s  got=%d", DMC_SLAVE_TX,
             got == 64 ? "通" : "✘ 不通", got);
    if (got == 64) ok++;

    /* B) 主机 → 从机：UART1 写，UART2 读（验 IO9 这条 pad）
     * ⚠️ 取样脚必须是主机**的 TX pad**。上一版误写成 DMC_UART_RX(IO8)，
     *    日志里打出「B) 发送中采样 IO8」就是征兆 —— 采的是被从机驱动的那根，
     *    等于用 A 的结论冒充 B。属于「用例打错靶子」，不是产品 bug。 */
    low = s_burst_and_count(DMC_UART_PORT, DMC_UART_TX, 7000);
    ESP_LOGW(TAG, "B) 发送中采样 IO%d 低 %d/7000 → %s", DMC_UART_TX, low,
             low > 500 ? "pad 确实在驱动，问题在**对端采不到**"
                       : "pad 发送期间无低电平");
    got = uart_read_bytes(DMC_SLAVE_PORT, buf, 64, pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "B) 主机→从机 (IO%d): %s  got=%d", DMC_UART_TX,
             got == 64 ? "通" : "✘ 不通", got);
    if (got == 64) ok++;

    s_dump_pad("afterA/B", DMC_SLAVE_TX);
    s_dump_pad("afterA/B", DMC_SLAVE_RX);

    uart_flush_input(DMC_UART_PORT);
    uart_flush_input(DMC_SLAVE_PORT);

    /* ---- 阳性对照：纯 GPIO 驱动（与 C0 构成两种驱动源的交叉验证） ---- */
    {
        int ctl;
        /* ⚠️ 阴性对照：IO10/IO11 全程没被本固件碰过，必为 oe=0 / outcfg 默认。
         *    上一版 dump 出「所有 pad 值一模一样、连 oe 都是 0」，可阳性对照
         *    明明刚把 IO9 拉低并真的驱动了 pad ⇒ **是读寄存器这一步本身有问题**
         *    （偏移/符号/位域），不是 pad 状态真的一致。
         *    用「本该不同的两根脚读出相同值」当场判读读者是否可信。 */
        s_dump_pad("never", 10);
        s_dump_pad("never", 11);
        ESP_LOGW(TAG, "[never   ] 原始值 en1=0x%08x ocfg8=0x%08x ocfg9=0x%08x ocfg17=0x%08x",
                 (unsigned)GPIO.enable1.val, (unsigned)GPIO.func_out_sel_cfg[8].val,
                 (unsigned)GPIO.func_out_sel_cfg[9].val, (unsigned)GPIO.func_out_sel_cfg[17].val);

        s_dump_pad("ctlPre", DMC_SLAVE_RX);
        gpio_set_direction(DMC_SLAVE_RX, GPIO_MODE_OUTPUT);
        s_dump_pad("ctlOE", DMC_SLAVE_RX);
        gpio_set_level((gpio_num_t)DMC_SLAVE_RX, 0);
        ctl = s_sample(DMC_SLAVE_RX, 200);
        gpio_set_level((gpio_num_t)DMC_SLAVE_RX, 1);
        ESP_LOGW(TAG, "阳性对照: 纯 GPIO 拉低 IO%d → 低 %d/200 ⇒ %s",
                 DMC_SLAVE_RX, ctl,
                 ctl > 100 ? "GPIO 驱动源可见"
                           : "⚠️ 连 GPIO 驱动源都读不到，采样完全失效");
    }

    ESP_LOGW(TAG, "==== 探针结论: %d/2 向通 %s ====", ok,
             ok == 2 ? "（免接线可行）"
                     : (ok == 0 ? "（两向皆断）" : "（单向断）"));
    return ok;
}
#endif
