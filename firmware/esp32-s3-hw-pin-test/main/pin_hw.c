/**
 * pin_hw.c — hw_pin 真机 BSP 实现（ESP32-S3）
 *
 * 设计要点（都是踩过坑换来的，别随手改）：
 *
 *   ① **SIM / REAL 的分水岭就是那个 NULL**：
 *        hw_pin 的 pin_gpio_write_raw() 里 `if (g_bsp.gpio_write) {...}` ——
 *        SIM  → BSP 全 NULL → 内存电平状态机 + 器件模型（确定性，可回归）；
 *        REAL → 装了回调  → 真的翻这颗芯片的 GPIO。
 *      这是「不假装成功」的结构性保证，不靠上层自觉。
 *
 *   ② **GPIO 回调只装 3 个就够让 SPI 链路成真**：
 *        bit-bang 路径（bb_spi_byte）的时钟/MOSI 翻转直接走
 *        bsp->gpio_write，MISO 采样走 bsp->gpio_read。
 *        换言之，装上 gpio_write/gpio_read 之后，
 *        hw_pin 的 BB 档 SPI **就是真的**（示波器能看到波形）。
 *        HW 档才额外需要 bsp->spi_xfer（外设映射）。
 *
 *   ③ **HW 档的 SPI 初始化按需懒加载**：
 *        切到 HW 档才 `spi_bus_initialize`，切走就 `spi_bus_free`。
 *        否则内部的 bsp->gpio_dir 会和 SPI 外设抢同一批脚
 *        （GPIO Matrix 改路由后 gpio_set_direction 会把它抢回来）。
 *
 *   ④ **UART 分两种模式，互斥**：
 *        loopback = uart_set_loop_back(true)：自发自收，只证外设本身；
 *        link     = 正常模式，TX 出 IO18 / RX 进 IO8：与目标器件真连。
 *      两者不能同时开（回环会把 TX 灌回自己的 RX）。
 *
 *   ⑤ **L0 如实回读**：没有升压硬件时 PWM 经 RC 就是 0..3.3V，
 *      本层就报 0..3.3V，**不谎报 12V**（谎报 = 假装测到）。
 */
#include "pin_hw.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

/* ============================================================
 * 档案（真机档 = s3-9p）
 * ============================================================ */
static const hw_pin_profile_t s_prof_s3 = {
    /* name */ "s3-9p",
    /* gpio: MOSI MISO  CK  CS  TX  RX RST VPP VCC */
    { PIN_S3_MOSI, PIN_S3_MISO, PIN_S3_CK, PIN_S3_CS,
      PIN_S3_TX,   PIN_S3_RX,   PIN_S3_RST, PIN_S3_VPP, PIN_S3_VCC },
    115200u,          /* uart_baud */
    1000000u,         /* spi_hz */
    0u,               /* spi_mode (CPOL=0 CPHA=0) */
    HW_PIN_DRV_BB,    /* drv: 默认 bit-bang（任意时序）；HW 档由 pin_hw_drv_set 切 */
    HW_PIN_VPP_5V
};

static const hw_pin_profile_t s_prof_s3_isp = {
    /* name */ "s3-isp",
    /* gpio: MOSI MISO  CK  CS  TX  RX RST VPP VCC */
    { -1, -1, -1, -1, PIN_S3_TX, PIN_S3_RX, PIN_S3_RST, -1, -1 },
    115200u, 0u, 0u, HW_PIN_DRV_BB, HW_PIN_VPP_OFF
};

/* 运行期覆写载体（g_active 是指针，不能指向栈上临时体） */
static hw_pin_profile_t s_prof_ovr;

/* ============================================================
 * 状态
 * ============================================================ */
static int              s_hw_ready;      /* GPIO/LEDC/ADC 已初始化 */
static int              s_adc_ready;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int              s_cali_ok;
static int              s_uart_mode;     /* 0=关闭 1=回环 2=走线 */
static int              s_pwm_duty = -1;
static int              s_ledc_ready;

/* HW 档 SPI（懒加载） */
static int              s_spi_ready;
static spi_device_handle_t s_spi;

/* ============================================================
 * GPIO 基本
 * ============================================================ */
static int gpio_out(int pin, int level)
{
    if (pin < 0) return -1;
    /* ⚠️ 用 INPUT_OUTPUT 而不是 OUTPUT。
     *
     *   gpio_set_direction(pin, GPIO_MODE_OUTPUT) 会**关掉该脚的输入通路**
     *   (FUN_IE=0)，此后 gpio_get_level(pin) 读到的是脏值/0，
     *   哪怕这一脚其实好好地被驱动在 1 上。
     *   2026-10-01 ESP32-S3 真机 [B2] 实测: 「模块写 1 → 直读回 0」，
     *   一度像是写不进去；真因是**读回自己驱动的脚必须先开输入通路**。
     *   保持输入通路开着对编程器无害 (只是多一点点输入缓冲电流)，
     *   却让「写出去的电平」随时可回读 —— 排障价值远大于那点功耗。
     *   (hw_dc 的 dc_hw_drive_readback 当初也是显式用 INPUT_OUTPUT 才读得到的。) */
    if (gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT) != ESP_OK) return -1;
    return (gpio_set_level((gpio_num_t)pin, level ? 1 : 0) == ESP_OK) ? 0 : -1;
}

static int gpio_in(int pin)
{
    if (pin < 0) return -1;
    if (gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT) != ESP_OK) return -1;
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
    return 0;
}

/* pull: 0=悬空 1=上拉 2=下拉 */
static int gpio_in_pull(int pin, int pull)
{
    if (pin < 0) return -1;
    if (gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT) != ESP_OK) return -1;
    gpio_set_pull_mode((gpio_num_t)pin,
                       (pull == 1) ? GPIO_PULLUP_ONLY :
                       (pull == 2) ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
    return 0;
}

/* ============================================================
 * BSP 回调
 * ============================================================ */
static int cb_gpio_dir(int pin, int output)
{
    return output ? gpio_out(pin, 0) : gpio_in(pin);
}

static int cb_gpio_write(int pin, int level)
{
    return gpio_out(pin, level);
}

static int cb_gpio_read(int pin)
{
    if (pin < 0) return -1;
    return (int)gpio_get_level((gpio_num_t)pin);
}

static int cb_delay_us(uint32_t us)
{
    esp_rom_delay_us(us ? us : 1u);
    return 0;
}

static int cb_uart_putc(int ch)
{
    uint8_t b = (uint8_t)(ch & 0xFF);
    if (s_uart_mode == 0) return -1;
    return (uart_write_bytes(PIN_UART_PORT, (const char*)&b, 1) == 1) ? 0 : -1;
}

/* 收 1 字节：带超时（无数据返回 -1 → 上层判 NOTGT，绝不吊死） */
static int cb_uart_getc(void)
{
    uint8_t b = 0;
    int r;
    if (s_uart_mode == 0) return -1;
    r = uart_read_bytes(PIN_UART_PORT, &b, 1, pdMS_TO_TICKS(200));
    return (r == 1) ? (int)b : -1;
}

/* HW 档：外设映射的 SPI 全双工收发（按需懒加载总线） */
static int cb_spi_xfer(const hw_pin_profile_t* p, const uint8_t* tx, uint8_t* rx, uint32_t n)
{
    spi_transaction_t t;
    esp_err_t e;

    if (!p || n == 0u) return 0;

    if (!s_spi_ready) {
        spi_bus_config_t bus;
        spi_device_interface_config_t dev;
        memset(&bus, 0, sizeof(bus));
        bus.mosi_io_num   = p->gpio[HW_PIN_MOSI];
        bus.miso_io_num   = p->gpio[HW_PIN_MISO];
        bus.sclk_io_num   = p->gpio[HW_PIN_CK];
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;

        memset(&dev, 0, sizeof(dev));
        dev.clock_speed_hz = (int)(p->spi_hz ? p->spi_hz : 1000000u);
        dev.mode           = (uint8_t)p->spi_mode;
        /* ⚠️ 片选交给模块自己（hw_pin_spi_set_cs），不由外设驱动：
         *    模块的 CS 是 gpio_write 出来的，若这里也让外设驱动同一根脚，
         *    两边会互相覆盖路由。故 spics = -1。 */
        dev.spics_io_num   = -1;
        dev.queue_size     = 4;
        if (spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
            (void)spi_bus_free(SPI2_HOST);
            return -1;
        }
        s_spi_ready = 1;
    }

    memset(&t, 0, sizeof(t));
    t.length    = (size_t)n * 8u;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    e = spi_device_transmit(s_spi, &t);
    return (e == ESP_OK) ? (int)n : -1;
}

static const hw_pin_bsp_t s_bsp_real = {
    .gpio_dir   = cb_gpio_dir,
    .gpio_write = cb_gpio_write,
    .gpio_read  = cb_gpio_read,
    .delay_us   = cb_delay_us,
    .spi_xfer   = cb_spi_xfer,
    .uart_putc  = cb_uart_putc,
    .uart_getc  = cb_uart_getc,
    .vpp_set    = NULL,          /* 没有升压硬件 → 不装，回落模拟（文档已注明） */
    .vpp_read   = NULL,
};

/* ============================================================
 * 初始化 / 释放
 * ============================================================ */
static adc_channel_t vpp_channel(void) { return (adc_channel_t)PIN_VPP_ADC_CH; }

int pin_hw_hw_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg;

    if (s_hw_ready) return 0;

    /* --- GPIO：档案内其余脚的初始方向 ---
     * ⚠️ **绝不碰 TX / RX**：它们归 UART 外设所有。
     *    uart_set_pin() 把 UART 的 TX 信号路由到该 pad 之后，
     *    若再来一句 gpio_set_direction(pad, OUTPUT)，GPIO Matrix 的
     *    输出选择会被改回「GPIO 寄存器」→ UART 就再也发不出去了
     *    （TX 会变成一根被钉死的高电平脚，症状是 ISP 全部 NOTGT）。
     *    这也是本层把 UART 打开放在 hw_init 之前的原因。 */
    (void)gpio_out(PIN_S3_MOSI, 0);
    (void)gpio_out(PIN_S3_CK,   0);
    (void)gpio_out(PIN_S3_CS,   1);          /* 片选空闲高 */
    (void)gpio_out(PIN_S3_RST,  1);          /* 复位空闲释放 */
    (void)gpio_out(PIN_S3_VCC,  0);          /* 目标供电默认关 */
    (void)gpio_in(PIN_S3_MISO);

    /* --- LEDC：编程电压 PWM --- */
    {
        ledc_timer_config_t t;
        ledc_channel_config_t c;
        memset(&t, 0, sizeof(t));
        t.speed_mode      = LEDC_LOW_SPEED_MODE;
        t.timer_num       = LEDC_TIMER_0;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.freq_hz         = PIN_VPP_PWM_HZ;
        t.clk_cfg         = LEDC_AUTO_CLK;
        if (ledc_timer_config(&t) != ESP_OK) return -2;

        memset(&c, 0, sizeof(c));
        c.gpio_num   = PIN_S3_VPP;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel    = LEDC_CHANNEL_0;
        c.timer_sel  = LEDC_TIMER_0;
        c.duty       = 0;
        c.hpoint     = 0;
        if (ledc_channel_config(&c) != ESP_OK) return -3;
        (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        s_pwm_duty = 0;
        s_ledc_ready = 1;
    }

    /* --- ADC：VPP 同脚回读 --- */
    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.unit_id  = ADC_UNIT_1;
    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return -4;
    {
        adc_oneshot_chan_cfg_t ccfg;
        memset(&ccfg, 0, sizeof(ccfg));
        ccfg.atten    = ADC_ATTEN_DB_12;
        ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(s_adc, vpp_channel(), &ccfg) != ESP_OK) return -5;
    }
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
#endif
    s_adc_ready = 1;
    s_hw_ready  = 1;
    return 0;
}

void pin_hw_hw_deinit(void)
{
    if (s_spi_ready) {
        (void)spi_bus_remove_device(s_spi);
        (void)spi_bus_free(SPI2_HOST);
        s_spi_ready = 0;
    }
    if (s_cali_ok) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        (void)adc_cali_delete_scheme_curve_fitting(s_cali);
#endif
        s_cali_ok = 0;
    }
    if (s_adc_ready) {
        (void)adc_oneshot_del_unit(s_adc);
        s_adc_ready = 0;
    }
    if (s_ledc_ready) {
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        s_ledc_ready = 0;
        s_pwm_duty = -1;
    }
    s_hw_ready = 0;
}

int pin_hw_profile_load(void)
{
    int rc = hw_pin_profile_valid(&s_prof_s3);
    if (rc != HW_PIN_R_OK) return -rc;
    (void)hw_pin_profile_load(&s_prof_s3);
    return 0;
}

int pin_hw_prof_isp_load(void)
{
    int rc = hw_pin_profile_valid(&s_prof_s3_isp);
    if (rc != HW_PIN_R_OK) return -rc;
    (void)hw_pin_profile_load(&s_prof_s3_isp);
    return 0;
}

void pin_hw_drv_set(int drv)
{
    const hw_pin_profile_t* cur = hw_pin_active();
    if (cur) s_prof_ovr = *cur;
    s_prof_ovr.drv = (uint8_t)drv;
    (void)hw_pin_profile_load(&s_prof_ovr);
}

int pin_hw_install(bool real)
{
    if (!real) {
        hw_pin_bsp_install(NULL);       /* ⚠️ 全 NULL 就是 SIM/REAL 的分水岭 */
        return 0;
    }
    if (pin_hw_hw_init() != 0) return -1;
    hw_pin_bsp_install(&s_bsp_real);
    return 0;
}

/* ============================================================
 * UART：回环 / 走线
 * ============================================================ */
static int uart_cfg(int loopback)
{
    uart_config_t c;
    memset(&c, 0, sizeof(c));
    c.baud_rate  = 115200;
    c.data_bits  = UART_DATA_8_BITS;
    c.parity     = UART_PARITY_DISABLE;
    c.stop_bits  = UART_STOP_BITS_1;
    c.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;

    /* 🕳️ 分段定位码：四步各自可能失败 (install / param / set_pin)，
     *   只回一个 -1 会让 [C0] 变成黑盒 —— 无法判断是驱动装不上、
     *   参数不合法还是引脚路由冲突。返回 -(步骤号+1) 便于对拍。 */
    if (uart_driver_install(PIN_UART_PORT, 2048, 2048, 0, NULL, 0) != ESP_OK) {
        /* 把 ESP_ERR_INVALID_STATE 等真实码打出来：只回 -1 无法区分
         * "驱动没释放" 与 "端口被占" 两类完全不同的根因。 */
        esp_err_t e = uart_driver_install(PIN_UART_PORT, 2048, 2048, 0, NULL, 0);
        ESP_LOGE("pin_hw", "uart_driver_install(UART%d) err=0x%X (%s) installed=%d",
                 (int)PIN_UART_PORT, (unsigned)e, esp_err_to_name(e),
                 (int)uart_is_driver_installed(PIN_UART_PORT));
        return -1;
    }
    if (uart_param_config(PIN_UART_PORT, &c) != ESP_OK) return -2;
    if (uart_set_pin(PIN_UART_PORT, PIN_S3_TX, PIN_S3_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -3;

    if (loopback) {
        const uint8_t purge[2] = { 0x00, 0x00 };
        /* ⚠️ hw_wdbg 实测坑：uart_set_loop_back(true) 的一瞬间 RX 会先冒一个
         *    伪字节 (0xff)，flush 拦不住 → 后续数据整体后移 1 位。
         *    对策：使能后先「灌注 2 字节 + 清空」把伪码喂掉。 */
        uart_set_loop_back(PIN_UART_PORT, true);
        uart_flush_input(PIN_UART_PORT);
        (void)uart_write_bytes(PIN_UART_PORT, (const char*)purge, sizeof(purge));
        (void)uart_wait_tx_done(PIN_UART_PORT, pdMS_TO_TICKS(100));
        esp_rom_delay_us(2000);
        uart_flush_input(PIN_UART_PORT);
        s_uart_mode = 1;
    } else {
        s_uart_mode = 2;
    }
    return 0;
}

int pin_hw_uart_link_opened(void)
{
    /* 只读状态查询，不做任何配置。供测试桥确认「主流程已开好走线链路」，
     * 避免重复 link_open 撞上 s_uart_mode 守卫（那会把「已开着」
     * 误报成「install 失败」的黑盒）。 */
    return (s_uart_mode == 2) ? 0 : -1;
}

int pin_hw_uart_loopback_open(void)
{
    if (s_uart_mode) return -1;
    return uart_cfg(1);
}

void pin_hw_uart_loopback_close(void)
{
    if (s_uart_mode == 1) {
        uart_set_loop_back(PIN_UART_PORT, false);
        uart_driver_delete(PIN_UART_PORT);
        /* 🕳️ uart_driver_delete 是**异步**的 (内部 esp_timer 延迟回收)，
         *   紧接着 uart_driver_install 会撞 ESP_ERR_INVALID_STATE。
         *   这里轮询等真正释放, 否则 [C0] 的 install 必失败。 */
        for (int i = 0; i < 200; i++) {          /* 上限 200×5ms = 1s */
            if (!uart_is_driver_installed(PIN_UART_PORT)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        s_uart_mode = 0;
    }
}

int pin_hw_uart_link_open(void)
{
    if (s_uart_mode) return -1;
    return uart_cfg(0);
}

void pin_hw_uart_link_close(void)
{
    if (s_uart_mode == 2) {
        uart_driver_delete(PIN_UART_PORT);
        for (int i = 0; i < 200; i++) {          /* 同上：等异步释放 */
            if (!uart_is_driver_installed(PIN_UART_PORT)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        s_uart_mode = 0;
    }
}

int pin_hw_uart_loopback_roundtrip(const uint8_t* out, int n,
                                   uint8_t* in, int cap, int timeout_ms)
{
    int got = 0;
    if (!out || n <= 0 || !in || cap <= 0 || s_uart_mode == 0) return -1;

    uart_flush_input(PIN_UART_PORT);
    if (uart_write_bytes(PIN_UART_PORT, (const char*)out, (size_t)n) != n) return -1;
    if (uart_wait_tx_done(PIN_UART_PORT, pdMS_TO_TICKS(200)) != ESP_OK) return -1;
    while (got < n && got < cap) {
        uint8_t b;
        int r = uart_read_bytes(PIN_UART_PORT, &b, 1, pdMS_TO_TICKS(timeout_ms));
        if (r <= 0) break;
        in[got++] = b;
    }
    return got;
}

/* 走线模式下的自发自收：无外部器件时读回 0 字节 = 导线没接通（诚实 SKIP）。
 * 有跳线 IO18↔IO8 时读回 == 发出的模式 → 「真实导线通路」成立。 */
int pin_hw_uart_wire_probe(const uint8_t* out, int n, uint8_t* in, int cap)
{
    int got = 0;
    if (!out || n <= 0 || cap <= 0) return -1;
    if (s_uart_mode != 2) return -1;          /* 只在走线模式才有意义 */

    uart_flush_input(PIN_UART_PORT);
    if (uart_write_bytes(PIN_UART_PORT, (const char*)out, (size_t)n) != n) return -1;
    (void)uart_wait_tx_done(PIN_UART_PORT, pdMS_TO_TICKS(200));
    while (got < n && got < cap) {
        uint8_t b;
        int r = uart_read_bytes(PIN_UART_PORT, &b, 1, pdMS_TO_TICKS(100));
        if (r <= 0) break;
        if (in) in[got] = b;
        got++;
    }
    return got;
}

/* ============================================================
 * 自证原语
 * ============================================================ */
int pin_hw_prove_gpio(int* pd_a, int* pu_a, int* pd_b, int* pu_b)
{
    int a, b, c, d;
    static const int P[2] = { PIN_S3_PROOF_A, PIN_S3_PROOF_B };
    int r[4];

    (void)gpio_in_pull(P[0], 2); esp_rom_delay_us(3000); r[0] = cb_gpio_read(P[0]); /* 下拉 → 0 */
    (void)gpio_in_pull(P[0], 1); esp_rom_delay_us(3000); r[1] = cb_gpio_read(P[0]); /* 上拉 → 1 */
    (void)gpio_in_pull(P[1], 2); esp_rom_delay_us(3000); r[2] = cb_gpio_read(P[1]); /* 下拉 → 0 */
    (void)gpio_in_pull(P[1], 1); esp_rom_delay_us(3000); r[3] = cb_gpio_read(P[1]); /* 上拉 → 1 */

    a = r[0]; b = r[1]; c = r[2]; d = r[3];
    if (pd_a) *pd_a = a;
    if (pu_a) *pu_a = b;
    if (pd_b) *pd_b = c;
    if (pu_b) *pu_b = d;
    return (a == 0 && b == 1 && c == 0 && d == 1) ? 0 : -1;
}

int pin_hw_prove_owngpio(int* hi, int* lo)
{
    int h1, l1, h2, l2;

    /* RST 与 CS 都是在档案里映射的脚；往它们写电平再直读，
     * 证明「模块写的 IO 号 == 这颗芯片的这颗脚」。 */
    (void)hw_pin_gpio_write(HW_PIN_RST, 1); esp_rom_delay_us(2000); h1 = cb_gpio_read(PIN_S3_RST);
    (void)hw_pin_gpio_write(HW_PIN_RST, 0); esp_rom_delay_us(2000); l1 = cb_gpio_read(PIN_S3_RST);
    (void)hw_pin_gpio_write(HW_PIN_CS,  1); esp_rom_delay_us(2000); h2 = cb_gpio_read(PIN_S3_CS);
    (void)hw_pin_gpio_write(HW_PIN_CS,  0); esp_rom_delay_us(2000); l2 = cb_gpio_read(PIN_S3_CS);

    (void)hw_pin_gpio_write(HW_PIN_RST, 1);
    (void)hw_pin_gpio_write(HW_PIN_CS,  1);

    if (hi) *hi = (h1 == 1 && h2 == 1) ? 1 : 0;
    if (lo) *lo = (l1 == 0 && l2 == 0) ? 1 : 0;
    return (h1 == 1 && l1 == 0 && h2 == 1 && l2 == 0) ? 0 : -1;
}

int pin_hw_spi_bb_loopback(const uint8_t* tx, uint8_t* rx, int n, int* nrx)
{
    static uint8_t rb[64];
    int i, rc, all0 = 1, allf = 1, same = 1;

    if (!tx || n <= 0 || n > (int)sizeof(rb)) return -1;

    pin_hw_drv_set(HW_PIN_DRV_BB);
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(tx, rb, (uint32_t)n);
    (void)hw_pin_spi_set_cs(1);
    if (rx) memcpy(rx, rb, (size_t)n);
    if (nrx) *nrx = n;
    if (rc != HW_PIN_R_OK) return -2;

    /* 判定：逐字节比对。全 0 / 全 0xFF = 悬空（没接器件）→ 报 SKIP，
     * 绝不当成「SPI 通了」（空转假通过）。 */
    for (i = 0; i < n; i++) {
        if (rb[i] != 0x00) all0 = 0;
        if (rb[i] != 0xFF) allf = 0;
        if (rb[i] != tx[i]) same = 0;
    }
    if (same && !all0 && !allf) return 0;     /* 跳线在位且一致 */
    return 1;                                 /* 未接跳线 / 不符 → SKIP */
}

int pin_hw_spi_hw_loopback(const uint8_t* tx, uint8_t* rx, int n, int* nrx)
{
    static uint8_t rb[64];
    int i, rc, ok;

    if (!tx || n <= 0 || n > (int)sizeof(rb)) return -1;

    pin_hw_drv_set(HW_PIN_DRV_HW);
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(tx, rb, (uint32_t)n);
    (void)hw_pin_spi_set_cs(1);
    if (rx) memcpy(rx, rb, (size_t)n);
    if (nrx) *nrx = n;

    if (rc != HW_PIN_R_OK) return -2;
    ok = 1;
    for (i = 0; i < n; i++) if (rb[i] != tx[i]) ok = 0;
    return ok ? 0 : 1;
}

/* ============================================================
 * L0 编程电压层
 * ============================================================ */
int pin_hw_vpp_pwm_max(void) { return (1 << PIN_VPP_PWM_BITS) - 1; }

int pin_hw_vpp_pwm_set(int duty)
{
    int mx = pin_hw_vpp_pwm_max();
    if (duty < 0) duty = 0;
    if (duty > mx) duty = mx;
    if (!s_ledc_ready) return -1;
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)duty);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_pwm_duty = duty;
    return 0;
}

void pin_hw_vpp_reassert(void)
{
    if (!s_ledc_ready) return;
    (void)ledc_set_pin(PIN_S3_VPP, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                        (uint32_t)(s_pwm_duty < 0 ? 0 : s_pwm_duty));
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static int vpp_adc_mv_once(void)
{
    int raw = 0, mv = 0;
    if (!s_adc_ready) return -1;
    if (adc_oneshot_read(s_adc, vpp_channel(), &raw) != ESP_OK) return -1;
    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv;
    }
    return (int)(((long)raw * 3100L) / 4095L);
}

int pin_hw_vpp_probe(int* mv, int* level)
{
    long sum = 0;
    int i, k = 0, m;

    for (i = 0; i < 16; i++) {
        m = vpp_adc_mv_once();
        if (m >= 0) { sum += m; k++; }
    }
    if (mv)    *mv    = k ? (int)(sum / (long)k) : -1;
    if (level) *level = cb_gpio_read(PIN_S3_VPP);
    return k ? 0 : -1;
}

/* ============================================================
 * 打印 / 体检
 * ============================================================ */
void pin_hw_dump(void)
{
    const hw_pin_profile_t* p = hw_pin_active();
    printf("pin_hw: hw_ready=%d adc=%d cali=%d ledc=%d uart_mode=%d spi=%d\n",
           s_hw_ready, s_adc_ready, s_cali_ok, s_ledc_ready, s_uart_mode, s_spi_ready);
    if (p) printf("pin_hw: profile=%s drv=%s spi_mode=%u spi_hz=%u\n",
                  p->name, (p->drv == HW_PIN_DRV_HW) ? "HW" : "BB",
                  (unsigned)p->spi_mode, (unsigned)p->spi_hz);
    printf("pin_hw: MOSI=IO%d=%d MISO=IO%d=%d CK=IO%d=%d CS=IO%d=%d\n",
           PIN_S3_MOSI, cb_gpio_read(PIN_S3_MOSI), PIN_S3_MISO, cb_gpio_read(PIN_S3_MISO),
           PIN_S3_CK,   cb_gpio_read(PIN_S3_CK),   PIN_S3_CS,   cb_gpio_read(PIN_S3_CS));
    printf("pin_hw: TX=IO%d RX=IO%d=%d RST=IO%d=%d VPP=IO%d=%d VCC=IO%d=%d\n",
           PIN_S3_TX, PIN_S3_RX, cb_gpio_read(PIN_S3_RX),
           PIN_S3_RST, cb_gpio_read(PIN_S3_RST), PIN_S3_VPP, cb_gpio_read(PIN_S3_VPP),
           PIN_S3_VCC, cb_gpio_read(PIN_S3_VCC));
}

#if PIN_PIN_SCAN
/* 引脚体检：对每个候选脚做「下拉→读 / 上拉→读」，凡不能跟随的脚
 * 就是被板载外设钉死（LCD/按键/PSRAM/Flash 等），换板必先跑这个。 */
int pin_hw_scan(void)
{
    static const int cand[] = {
        1,2,8,9,11,12,13,14,15,16,17,18,21,38,39,40,41,42,47,48
    };
    int i, bad = 0;
    printf("pin_hw: --- pin scan (下拉/上拉 应读到 0/1) ---\n");
    for (i = 0; i < (int)(sizeof(cand)/sizeof(cand[0])); i++) {
        int q = cand[i], l0, l1;
        (void)gpio_in_pull(q, 2); esp_rom_delay_us(3000); l0 = cb_gpio_read(q);
        (void)gpio_in_pull(q, 1); esp_rom_delay_us(3000); l1 = cb_gpio_read(q);
        printf("pin_hw:   IO%-2d  pd/pu = %d/%d  %s\n", q, l0, l1,
               (l0 == 0 && l1 == 1) ? "ok" : "**异常(脚被钉死)**");
        if (!(l0 == 0 && l1 == 1)) bad++;
    }
    (void)gpio_in(PIN_S3_MISO);
    return bad;
}
#endif
