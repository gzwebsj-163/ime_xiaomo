/**
 * prog_hw.c — 烧录器真机 BSP 实现（ESP32-S3）
 *
 * 本板脚位（避开已占用/危险脚，依据 btn.h 实测清单）：
 *   ┌────────┬───────┬──────────────────────────────────────────────┐
 *   │ 信号   │ GPIO  │ 说明                                          │
 *   ├────────┼───────┼──────────────────────────────────────────────┤
 *   │ MOSI   │ 11    │ SPI 主出                                      │
 *   │ MISO   │ 13    │ SPI 主入（配内部上拉 → 悬空读 1，不假装有芯片）│
 *   │ CK     │ 12    │ SPI 时钟                                      │
 *   │ CS     │ 14    │ 片选，空闲拉高（未选中）                       │
 *   │ TX     │ 8     │ UART1 发（GPIO Matrix 任意路由）               │
 *   │ RX     │ 9     │ UART1 收                                      │
 *   │ RST    │ 18    │ 目标复位 / 进编程模式                          │
 *   │ VPP    │ –     │ 本版无升压硬件，未映射（档位 API 仍在）        │
 *   │ VCC    │ –     │ 本版由目标自供电，未映射                       │
 *   └────────┴───────┴──────────────────────────────────────────────┘
 * 已占用（不可用）：0/3/45/46 strapping · 19/20 USB-JTAG · 26~32 片内 Flash
 *                  33~37 八线 PSRAM · 43/44 UART0 控制台 · 4/5/6/7/10 LCD
 *                  38/39/17 按键
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"

#include "prog_hw.h"

/* ---- 本板引脚（换线只改这一段）---- */
#define S3_MOSI 11
#define S3_MISO 13
#define S3_CK   12
#define S3_CS   14
#define S3_TX    8
#define S3_RX    9
#define S3_RST  18

#define PROG_UART_NUM   UART_NUM_1
#define PROG_UART_RXBUF 512

/* ---- 三条档案：gpio[] 顺序 = MOSI MISO CK CS TX RX RST VPP VCC ---- */
static const hw_pin_profile_t s_prof_spi = {
    "s3-spi",
    { S3_MOSI, S3_MISO, S3_CK, S3_CS, S3_TX, S3_RX, S3_RST, -1, -1 },
    115200u, 1000000u, 0u, HW_PIN_DRV_BB, HW_PIN_VPP_OFF
};
static const hw_pin_profile_t s_prof_w25q = {
    "w25q",
    { S3_MOSI, S3_MISO, S3_CK, S3_CS, -1, -1, -1, -1, -1 },
    0u, 1000000u, 0u, HW_PIN_DRV_BB, HW_PIN_VPP_OFF
};
static const hw_pin_profile_t s_prof_isp = {
    "s3-isp",
    { -1, -1, -1, -1, S3_TX, S3_RX, S3_RST, -1, -1 },
    115200u, 0u, 0u, HW_PIN_DRV_BB, HW_PIN_VPP_OFF
};

const hw_pin_profile_t *prog_hw_profile_spi(void)  { return &s_prof_spi; }
const hw_pin_profile_t *prog_hw_profile_w25q(void) { return &s_prof_w25q; }
const hw_pin_profile_t *prog_hw_profile_isp(void)  { return &s_prof_isp; }

const hw_pin_profile_t *prog_hw_profile_by_name(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "s3-spi") == 0) return &s_prof_spi;
    if (strcmp(name, "w25q")   == 0) return &s_prof_w25q;
    if (strcmp(name, "s3-isp") == 0) return &s_prof_isp;
    return NULL;
}

/* ============================================================
 * 真机 BSP 回调
 * ============================================================ */
static int s_real  = 0;      /* 1 = 当前装的是真机 BSP */
static int s_uart_on = 0;

static int cfg_out(int pin, int init_level)
{
    gpio_config_t c;
    if (pin < 0) return 0;                  /* 未映射 = 跳过，不是错 */
    memset(&c, 0, sizeof(c));
    c.pin_bit_mask = 1ULL << (unsigned)pin;
    c.mode         = GPIO_MODE_OUTPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    if (gpio_config(&c) != ESP_OK) return -1;
    gpio_set_level((gpio_num_t)pin, init_level ? 1 : 0);
    return 0;
}

static int cfg_in(int pin, int pullup)
{
    gpio_config_t c;
    if (pin < 0) return 0;
    memset(&c, 0, sizeof(c));
    c.pin_bit_mask = 1ULL << (unsigned)pin;
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    return (gpio_config(&c) == ESP_OK) ? 0 : -1;
}

static void uart_close(void)
{
    if (s_uart_on) {
        (void)uart_driver_delete(PROG_UART_NUM);
        s_uart_on = 0;
    }
}

static int uart_open(int tx, int rx, uint32_t baud)
{
    uart_config_t uc;
    if (tx < 0 && rx < 0) return 0;         /* 该档案没接 UART */
    if (baud == 0u) baud = 115200u;

    memset(&uc, 0, sizeof(uc));
    uc.baud_rate  = (int)baud;
    uc.data_bits  = UART_DATA_8_BITS;
    uc.parity     = UART_PARITY_DISABLE;
    uc.stop_bits  = UART_STOP_BITS_1;
    uc.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;

    uart_close();                            /* 幂等：先关再开，避免 INVALID_STATE */
    if (uart_param_config(PROG_UART_NUM, &uc) != ESP_OK) return -1;
    /* GPIO Matrix：TX/RX 路由到任意脚（本方案能成立的关键红利） */
    if (uart_set_pin(PROG_UART_NUM,
                     (tx >= 0) ? tx : UART_PIN_NO_CHANGE,
                     (rx >= 0) ? rx : UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;
    if (uart_driver_install(PROG_UART_NUM, PROG_UART_RXBUF, PROG_UART_RXBUF, 0, NULL, 0) != ESP_OK)
        return -1;
    uart_flush_input(PROG_UART_NUM);
    s_uart_on = 1;
    return 0;
}

/* ---- hw_pin 认识的 9 个回调 ---- */
static int bsp_gpio_dir(int pin, int output)
{
    return (output ? cfg_out(pin, 0) : cfg_in(pin, 0));
}

static int bsp_gpio_write(int pin, int level)
{
    if (pin < 0) return -1;
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
    return 0;
}

static int bsp_gpio_read(int pin)
{
    if (pin < 0) return -1;
    return gpio_get_level((gpio_num_t)pin);
}

static int bsp_delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
    return 0;
}

static int bsp_uart_putc(int ch)
{
    uint8_t b = (uint8_t)ch;
    if (!s_uart_on) return -1;
    if (uart_write_bytes(PROG_UART_NUM, &b, 1) != 1) return -1;
    /* ISP 是半双工一问一答：必须等这一字节真的发完再去读，否则会读到自己的回显 */
    (void)uart_wait_tx_done(PROG_UART_NUM, pdMS_TO_TICKS(50));
    return 0;
}

static int bsp_uart_getc(void)
{
    uint8_t b = 0;
    int n;
    if (!s_uart_on) return -1;
    n = uart_read_bytes(PROG_UART_NUM, &b, 1, pdMS_TO_TICKS(150));
    return (n == 1) ? (int)b : -1;
}

static const hw_pin_bsp_t s_bsp_real = {
    bsp_gpio_dir,
    bsp_gpio_write,
    bsp_gpio_read,
    bsp_delay_us,
    NULL,               /* spi_xfer = NULL → 强制走 bit-bang（HW 模式留待后续版本） */
    bsp_uart_putc,
    bsp_uart_getc,
    NULL,               /* vpp_set  = NULL → 本版无升压硬件 */
    NULL                /* vpp_read = NULL → 回落到模型值，不假装测到真电压 */
};

/* ============================================================
 * 装载
 * ============================================================ */
static int setup_pins(const hw_pin_profile_t *p)
{
    int rc = 0;
    /* 输出脚：MOSI/CK 空闲低（SPI mode0）、CS 空闲高（未选中）、RST 空闲高（不复位） */
    rc |= cfg_out(p->gpio[HW_PIN_MOSI], 0);
    rc |= cfg_out(p->gpio[HW_PIN_CK],   0);
    rc |= cfg_out(p->gpio[HW_PIN_CS],   1);
    rc |= cfg_out(p->gpio[HW_PIN_RST],  1);
    rc |= cfg_out(p->gpio[HW_PIN_VPP],  0);
    rc |= cfg_out(p->gpio[HW_PIN_VCC],  0);
    /* MISO：输入 + 上拉。悬空时读 1（与真闪存未选中时的高电平一致）→
     * JEDEC 读到 0xFF → 诚实报 NOFLASH，绝不假装有芯片。 */
    rc |= cfg_in(p->gpio[HW_PIN_MISO], 1);

    if (uart_open((int)p->gpio[HW_PIN_TX], (int)p->gpio[HW_PIN_RX], p->uart_baud) != 0) rc |= 1;
    return rc ? -1 : 0;
}

int prog_hw_install(const hw_pin_profile_t *p, bool real)
{
    int rc = hw_pin_profile_load(p);
    if (rc != HW_PIN_R_OK) return rc;

    uart_close();

    if (!real) {
        /* ⚠️ 这一个 NULL 就是 SIM/REAL 的分水岭：
         *    hw_pin 的 bit-bang 里 use_dev = (g_bsp.gpio_read == NULL)，
         *    全 NULL → 器件模型供 MISO → RDID 稳定 EF 40 18。 */
        hw_pin_bsp_install(NULL);
        s_real = 0;
        return HW_PIN_R_OK;
    }

    if (setup_pins(p) != 0) return HW_PIN_R_IOERR;
    hw_pin_bsp_install(&s_bsp_real);
    s_real = 1;
    return HW_PIN_R_OK;
}

void prog_hw_release(void)
{
    uart_close();
    hw_pin_bsp_install(NULL);
    s_real = 0;
}

/* ============================================================
 * 真机自证：MOSI↔MISO 跳线回环
 * 走的就是生产路径 hw_pin_spi_xfer（REAL → bb_spi_byte → 真 GPIO），
 * 所以它能同时证明：引脚档案对、方向对不对、时序跑得动、接线通。
 * ============================================================ */
int prog_hw_loopback(uint8_t *tx_out, uint8_t *rx_out, uint32_t *n_out)
{
    static const uint8_t pat[8] = { 0x9F, 0xA5, 0x00, 0xFF, 0x5A, 0x3C, 0xC3, 0x81 };
    static uint8_t rx[8];
    uint32_t i;

    if (hw_pin_gpio_of(HW_PIN_MOSI) < 0 ||
        hw_pin_gpio_of(HW_PIN_MISO) < 0 ||
        hw_pin_gpio_of(HW_PIN_CK)   < 0)
        return -1;

    memset(rx, 0, sizeof(rx));
    if (hw_pin_spi_xfer(pat, rx, (uint32_t)sizeof(pat)) != HW_PIN_R_OK) return -2;

    if (tx_out) memcpy(tx_out, pat, sizeof(pat));
    if (rx_out) memcpy(rx_out, rx, sizeof(rx));
    if (n_out)  *n_out = (uint32_t)sizeof(pat);

    for (i = 0; i < (uint32_t)sizeof(pat); i++)
        if (rx[i] != pat[i]) return (int)i + 1;   /* 返回首个错位（1 基） */
    return 0;
}

void prog_hw_dump(void)
{
    const hw_pin_profile_t *p = hw_pin_active();
    int i;
    if (!p) return;
    printf("prog_hw: profile=%s drv=%s real=%d spi_mode=%u uart=%u\n",
           p->name, (p->drv == HW_PIN_DRV_HW) ? "HW" : "BB",
           s_real, (unsigned)p->spi_mode, (unsigned)p->uart_baud);
    printf("prog_hw: ");
    for (i = 0; i < HW_PIN_SIG_MAX; i++)
        if (p->gpio[i] >= 0) printf("%s=IO%d ", hw_pin_sig_name(i), (int)p->gpio[i]);
    printf("GND=GND\n");
}
