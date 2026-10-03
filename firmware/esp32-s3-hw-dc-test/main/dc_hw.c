/**
 * dc_hw.c — hw_dc 真机 BSP 实现（ESP32-S3）
 *
 * 设计要点（都是踩过坑换来的，别随手改）：
 *
 *   ① **SIM / REAL 的分水岭就是那个 NULL**：
 *        hw_dc 的 `hw_dc_sig_value()` 里 `if (g_bsp.read) {...}` ——
 *        SIM  → BSP 全 NULL → 读模块内信号表（确定性，可回归）；
 *        REAL → 装了 read  → 读片上 ADC / GPIO。
 *      这是「不假装成功」的结构性保证，不靠上层自觉。
 *
 *   ② **极值由 BSP 合成**：MAXIN/MININ/MAXOUT/MINOUT 硬件上不存在，
 *      由本层在每次 read() 时维护运行期 min/max。映射不了的不假装映射。
 *
 *   ③ **base_read 有意不装**：本版没有参考分压网络，
 *      让模块回落到内置参考预值（IN 0x059 / OUT 0x0080）。
 *      有参考硬件时再注入 —— 现在注入一个假值就是「假装测到」。
 *
 *   ④ **ADC 脚必须 GPIO_FLOATING**（数字侧悬空，不带上拉）。
 *
 *   ⑤ **自证策略（本层最关键的取舍，见 dc_hw.h 文件头）**：
 *      数字通路 → 内部上下拉自己跳变，**不需外部器件**（硬证据）；
 *      ADC 通路 → 弱上下拉不生效、本脚自驱动又被 adc_oneshot 抢占，
 *                 故 **必须一根跳线**（非 ADC 脚驱动 → ADC 读），无跳线只报 SKIP。
 *      ⚠️ 绝不用「采样噪声漂移」冒充 ADC 读数正确 —— 那是空转假通过。
 */
#include "dc_hw.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"

/* ============================================================
 * 状态
 * ============================================================ */
static int                       s_real;        /* 1 = 真机 BSP 已装 */
static int                       s_adc_ready;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int                       s_cali_ok;

/* 读取历程（索引 = 侧别 0=IN 1=OUT）*/
#define DC_HIST_N 2
static int s_hist_min[DC_HIST_N] = {  0x7FFFFFFF,  0x7FFFFFFF };
static int s_hist_max[DC_HIST_N] = { -0x7FFFFFFF, -0x7FFFFFFF };

/* ============================================================
 * GPIO / ADC 基本
 * ============================================================ */
static void settle(void) { esp_rom_delay_us(5000); }

static int pin_cfg_out(int pin, int level)
{
    gpio_config_t c;
    if (pin < 0) return 0;
    memset(&c, 0, sizeof(c));
    c.pin_bit_mask = 1ULL << (unsigned)pin;
    c.mode         = GPIO_MODE_OUTPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&c) != ESP_OK) return -1;
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
    return 0;
}

/* pull: 0=悬空 1=上拉 2=下拉 */
static int pin_cfg_in(int pin, int pull)
{
    gpio_config_t c;
    if (pin < 0) return 0;
    memset(&c, 0, sizeof(c));
    c.pin_bit_mask = 1ULL << (unsigned)pin;
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = (pull == 1) ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
    c.pull_down_en = (pull == 2) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    return (gpio_config(&c) == ESP_OK) ? 0 : -1;
}

static int pin_get(int pin)
{
    if (pin < 0) return -1;
    return gpio_get_level((gpio_num_t)pin);
}

/* ADC 脚专用：数字输入 + 悬空（不带上拉，否则读数被拉偏）*/
static void adc_pin_cfg(int pin)
{
    if (pin < 0) return;
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
}

/* 本板 ADC1: GPIO1..GPIO10 == ADC1_CH0..CH9 */
static adc_channel_t pin_channel(int pin)
{
    int ch = pin - 1;
    if (ch < 0) ch = 0;
    if (ch > 9) ch = 9;
    return (adc_channel_t)ch;
}

static adc_channel_t side_channel(uint8_t side)
{
    return (side == DC_SIDE_OUT) ? pin_channel(DC_PIN_ADC_OUT)
                                 : pin_channel(DC_PIN_ADC_IN);
}

/* ============================================================
 * 初始化 / 释放
 * ============================================================ */
static int adc_chan_setup(adc_channel_t ch)
{
    adc_oneshot_chan_cfg_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.atten    = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    return (adc_oneshot_config_channel(s_adc, ch, &ccfg) == ESP_OK) ? 0 : -1;
}

int dc_hw_hw_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg;

    if (s_adc_ready) return 0;

    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.unit_id  = ADC_UNIT_1;
    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return -1;
    if (adc_chan_setup(pin_channel(DC_PIN_ADC_OUT)) != 0) return -2;
    if (adc_chan_setup(pin_channel(DC_PIN_ADC_IN))  != 0) return -3;

    /* 校准方案：有就用（S3 走 curve fitting），没有就兜底线性换算 */
    s_cali_ok = 0;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cal;
        memset(&cal, 0, sizeof(cal));
        cal.unit_id  = ADC_UNIT_1;
        cal.atten    = ADC_ATTEN_DB_12;
        cal.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK) s_cali_ok = 1;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cal;
        memset(&cal, 0, sizeof(cal));
        cal.unit_id  = ADC_UNIT_1;
        cal.atten    = ADC_ATTEN_DB_12;
        cal.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) == ESP_OK) s_cali_ok = 1;
    }
#endif

    (void)pin_cfg_out(DC_PIN_PROBE, 0);        /* 回环探针：空闲低 */
    (void)pin_cfg_in(DC_PIN_HILITE, 2);        /* 高电平信号位：默认下拉 */
    (void)pin_cfg_in(DC_PIN_LOLITE, 1);        /* 低电平信号位：默认上拉 */
    adc_pin_cfg(DC_PIN_ADC_IN);                /* 模拟输入：数字悬空 */
    adc_pin_cfg(DC_PIN_ADC_OUT);

    s_hist_min[0] = 0x7FFFFFFF; s_hist_max[0] = -0x7FFFFFFF;
    s_hist_min[1] = 0x7FFFFFFF; s_hist_max[1] = -0x7FFFFFFF;

    s_adc_ready = 1;
    return 0;
}

void dc_hw_hw_deinit(void)
{
    if (s_cali_ok) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        (void)adc_cali_delete_scheme_curve_fitting(s_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        (void)adc_cali_delete_scheme_line_fitting(s_cali);
#endif
        s_cali_ok = 0;
    }
    if (s_adc_ready) {
        (void)adc_oneshot_del_unit(s_adc);
        s_adc_ready = 0;
    }
}

/* ============================================================
 * 直读接口（绕开模块，给验证桥做对拍锚点）
 * ============================================================ */
static int adc_ch_raw(adc_channel_t ch)
{
    int raw = 0;
    if (!s_adc_ready) return -1;
    if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) return -1;
    return raw;
}

static int adc_ch_mv(adc_channel_t ch)
{
    int raw = adc_ch_raw(ch);
    int mv = 0;
    if (raw < 0) return -1;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    }
    /* 兜底：12dB 衰减满量程约 3100mV 的线性近似 */
    return (int)(((long)raw * 3100L) / (long)DC_ADC_RAW_MAX);
}

int dc_hw_adc_raw(uint8_t side) { return adc_ch_raw(side_channel(side)); }
int dc_hw_adc_mv(uint8_t side)  { return adc_ch_mv(side_channel(side)); }

static int adc_avg_ch(adc_channel_t ch, int n)
{
    long sum = 0;
    int i, k = 0;
    for (i = 0; i < n; i++) {
        int mv = adc_ch_mv(ch);
        if (mv >= 0) { sum += mv; k++; }
    }
    return k ? (int)(sum / (long)k) : -1;
}

/* ============================================================
 * BSP 回调：DC 信号 → 真实读数
 * ============================================================ */
static int dc_hw_read(int sig)
{
    int side, mv;

    switch (sig) {
    case DC_INPUT:  case DC_MAXIN:  case DC_MININ:  side = (int)DC_SIDE_IN;  break;
    case DC_OUTPUT: case DC_MAXOUT: case DC_MINOUT: side = (int)DC_SIDE_OUT; break;
    /* 数字位：比较器 / 中继 / CHRG 类开漏信号，直读引脚电平 */
    case DC_HIGH_LITE: return pin_get(DC_PIN_HILITE);
    case DC_LOW_LITE:  return pin_get(DC_PIN_LOLITE);
    default:           return -1;
    }

    mv = dc_hw_adc_mv((uint8_t)side);
    if (mv < 0) return -1;                       /* 读失败 → 上层落 IOERR/越界 */

    if (mv < s_hist_min[side]) s_hist_min[side] = mv;
    if (mv > s_hist_max[side]) s_hist_max[side] = mv;

    switch (sig) {
    case DC_MAXIN:  case DC_MAXOUT: return s_hist_max[side];
    case DC_MININ:  case DC_MINOUT: return s_hist_min[side];
    default:                        return mv;
    }
}

static const hw_dc_bsp_t s_bsp_real = {
    dc_hw_read,
    NULL        /* base_read = NULL → 回落内置参考预值（本版无参考分压网络） */
};

/* ============================================================
 * 装载 / 释放
 * ============================================================ */
int dc_hw_install(bool real)
{
    if (!real) {
        /* ⚠️ 全 NULL 就是 SIM/REAL 的分水岭 */
        hw_dc_bsp_install(NULL);
        s_real = 0;
        return 0;
    }
    if (dc_hw_hw_init() != 0) return -1;
    hw_dc_bsp_install(&s_bsp_real);
    s_real = 1;
    return 0;
}

/* ============================================================
 * 真实性自证原语
 * ============================================================ */
int dc_hw_prove_digital(int *pd_hi, int *pu_hi, int *pu_lo, int *pd_lo)
{
    int a, b, c, d;

    (void)pin_cfg_in(DC_PIN_HILITE, 2); settle(); a = pin_get(DC_PIN_HILITE);  /* 下拉 → 0 */
    (void)pin_cfg_in(DC_PIN_HILITE, 1); settle(); b = pin_get(DC_PIN_HILITE);  /* 上拉 → 1 */
    (void)pin_cfg_in(DC_PIN_LOLITE, 1); settle(); c = pin_get(DC_PIN_LOLITE);  /* 上拉 → 1 */
    (void)pin_cfg_in(DC_PIN_LOLITE, 2); settle(); d = pin_get(DC_PIN_LOLITE);  /* 下拉 → 0 */

    (void)pin_cfg_in(DC_PIN_HILITE, 2);        /* 还原默认态 */
    (void)pin_cfg_in(DC_PIN_LOLITE, 1);

    if (pd_hi) *pd_hi = a;
    if (pu_hi) *pu_hi = b;
    if (pu_lo) *pu_lo = c;
    if (pd_lo) *pd_lo = d;

    return (a == 0 && b == 1 && c == 1 && d == 0) ? 0 : -1;
}

/* 存活体检（软证据）：只证明 ADC 在采样且回报合法值，不证明读数正确 */
int dc_hw_adc_sane(int *vmin, int *vmax, int *vavg)
{
    adc_channel_t ch = side_channel(DC_SIDE_IN);
    long sum = 0;
    int i, k = 0, lo = 0x7FFFFFFF, hi = -0x7FFFFFFF, mv;

    adc_pin_cfg(DC_PIN_ADC_IN);
    for (i = 0; i < 16; i++) {
        mv = adc_ch_mv(ch);
        if (mv < 0) return -1;
        if (mv < 0 || mv > 3300) return -2;       /* 超出量程 = 不正常 */
        if (mv < lo) lo = mv;
        if (mv > hi) hi = mv;
        sum += mv; k++;
    }
    if (vmin) *vmin = lo;
    if (vmax) *vmax = hi;
    if (vavg) *vavg = (int)(sum / (long)k);
    return 0;
}

/* ADC 硬自证：需要跳线 PROBE(纯数字脚) ↔ ADC_IN(ADC 脚)。
 * PROBE 未被 ADC 占用 → 输出驱动有效 → 能真正把 ADC 脚拉高/拉低。 */
int dc_hw_probe_loopback(int *mv_high, int *mv_low)
{
    int h, l;

    (void)pin_cfg_out(DC_PIN_PROBE, 1); esp_rom_delay_us(20000); h = adc_avg_ch(side_channel(DC_SIDE_IN), 8);
    (void)pin_cfg_out(DC_PIN_PROBE, 0); esp_rom_delay_us(20000); l = adc_avg_ch(side_channel(DC_SIDE_IN), 8);

    if (mv_high) *mv_high = h;
    if (mv_low)  *mv_low  = l;

    if (h < 0 || l < 0) return -1;
    return (h - l > 1000) ? 0 : 1;      /* 0 = 跳线在位；1 = 无跳线（SKIP） */
}

/* ============================================================
 * Phase C: DCPP 帧层真机传输（UART1 内部回环）
 * ============================================================ */

/* ⚠️ hw_wdbg 实测坑：uart_set_loop_back(enable) 的一瞬间 RX 会先冒一个伪字节
 *    (0xff)，flush 拦不住 → 会让后续数据整体后移 1 位。
 *    对策：使能后先「灌注 2 字节 0x00 + 清空」把伪码喂掉，再开始正式传输。 */
int dc_hw_uart_open(void)
{
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const uint8_t purge[2] = { 0x00, 0x00 };

    if (uart_driver_install(DC_UART_PORT, 1024, 1024, 0, NULL, 0) != ESP_OK) return -1;
    if (uart_param_config(DC_UART_PORT, &cfg) != ESP_OK) return -1;
    if (uart_set_pin(DC_UART_PORT, DC_UART_TX, DC_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;

    uart_set_loop_back(DC_UART_PORT, true);

    /* 喂掉使能瞬间的伪码 */
    uart_flush_input(DC_UART_PORT);
    uart_write_bytes(DC_UART_PORT, (const char*)purge, sizeof(purge));
    uart_wait_tx_done(DC_UART_PORT, pdMS_TO_TICKS(100));
    esp_rom_delay_us(2000);
    uart_flush_input(DC_UART_PORT);
    return 0;
}

void dc_hw_uart_close(void)
{
    uart_set_loop_back(DC_UART_PORT, false);
    uart_driver_delete(DC_UART_PORT);
}

int dc_hw_uart_roundtrip(const uint8_t* out, int n,
                         uint8_t* in, int cap, int timeout_ms)
{
    int got = 0;

    if (out == NULL || n <= 0 || in == NULL || cap <= 0) return -1;

    /* 每帧前清空输入，保证读到的是本帧（回环下不会有外来数据，但要防上一帧残留） */
    uart_flush_input(DC_UART_PORT);

    if (uart_write_bytes(DC_UART_PORT, (const char*)out, (size_t)n) != n) return -1;
    if (uart_wait_tx_done(DC_UART_PORT, pdMS_TO_TICKS(100)) != ESP_OK) return -1;

    /* 收齐 n 字节（或 cap 上限）；每字节等 timeout_ms */
    while (got < n && got < cap) {
        uint8_t b;
        int r = uart_read_bytes(DC_UART_PORT, &b, 1, pdMS_TO_TICKS(timeout_ms));
        if (r <= 0) break;                 /* 超时/无数据 → 结束 */
        in[got++] = b;
    }
    return got;
}

void dc_hw_dump(void)
{
    printf("dc_hw: real=%d adc_ready=%d cali=%d\n", s_real, s_adc_ready, s_cali_ok);
    printf("dc_hw: IN raw=%d mv=%d | OUT raw=%d mv=%d\n",
           dc_hw_adc_raw(DC_SIDE_IN),  dc_hw_adc_mv(DC_SIDE_IN),
           dc_hw_adc_raw(DC_SIDE_OUT), dc_hw_adc_mv(DC_SIDE_OUT));
    printf("dc_hw: pins  HILITE=IO%d=%d  LOLITE=IO%d=%d  PROBE=IO%d  ADC_IN=IO%d  ADC_OUT=IO%d\n",
           DC_PIN_HILITE, pin_get(DC_PIN_HILITE),
           DC_PIN_LOLITE, pin_get(DC_PIN_LOLITE),
           DC_PIN_PROBE, DC_PIN_ADC_IN, DC_PIN_ADC_OUT);
}

/* ============================================================
 * 板级 bring-up（仅 -DDC_PIN_SCAN=1 编译；换板排查用）
 * ============================================================ */
#if DC_PIN_SCAN
/* 驱动+回读对照：唯一变量 = 该脚是否被 adc_oneshot 配置过。
 * 结论（本板实测）：未配置 → 1/0；已配置 → 0/0
 *   ⇒ adc_oneshot 配置通道会独占 pad 并关掉数字输出驱动器。 */
void dc_hw_drive_readback(void)
{
    static const int ctl[5] = { DC_PIN_HILITE, DC_PIN_LOLITE, 2, 1, DC_PIN_ADC_IN };
    int i;
    for (i = 0; i < 5; i++) {
        int q = ctl[i], hi, lo;
        gpio_set_direction((gpio_num_t)q, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)q, GPIO_FLOATING);
        gpio_set_level((gpio_num_t)q, 1); esp_rom_delay_us(3000); hi = gpio_get_level((gpio_num_t)q);
        gpio_set_level((gpio_num_t)q, 0); esp_rom_delay_us(3000); lo = gpio_get_level((gpio_num_t)q);
        gpio_set_direction((gpio_num_t)q, GPIO_MODE_INPUT);
        printf("dc_hw: 对照 drive-readback IO%-2d => hi/lo = %d/%d\n", q, hi, lo);
    }
}

/* ADC 脚体检：GPIO1..10 逐个试，报告分离度（挑干净脚用） */
int dc_hw_scan_adc(void)
{
    int p, best = -1, best_sep = -1;

    if (!s_adc_ready) return -1;

    printf("dc_hw: --- ADC pin scan (GPIO1..10) ---\n");
    for (p = 1; p <= 10; p++) {
        adc_channel_t ch = pin_channel(p);
        int dh, dl, sep, dhi, dlo;

        if (adc_chan_setup(ch) != 0) continue;

        gpio_set_direction((gpio_num_t)p, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)p, GPIO_FLOATING);
        gpio_set_level((gpio_num_t)p, 1); esp_rom_delay_us(20000);
        dhi = gpio_get_level((gpio_num_t)p);
        dh  = adc_avg_ch(ch, 8);
        gpio_set_level((gpio_num_t)p, 0); esp_rom_delay_us(20000);
        dlo = gpio_get_level((gpio_num_t)p);
        dl  = adc_avg_ch(ch, 8);
        adc_pin_cfg(p);

        sep = (dh >= 0 && dl >= 0) ? (dh - dl) : -1;
        printf("dc_hw:   IO%-2d (CH%d)  adc hi/lo=%4d/%4d sep=%4d | 引脚数字 hi/lo=%d/%d\n",
               p, (int)ch, dh, dl, sep, dhi, dlo);
        if (sep > best_sep) { best_sep = sep; best = p; }
    }
    printf("dc_hw: scan best = IO%d (sep=%d mV, 注意: 已声称通道的脚驱动无效, sep 含噪声)\n",
           best, best_sep);
    return best;
}
#endif /* DC_PIN_SCAN */
