/**
 * pin_hw.h — hw_pin 真机 BSP（ESP32-S3）：真实引脚 / SPI / UART / 编程电压
 *
 * 这一层是 hw_pin 的「从档案驱动到真实硅」那一跳：
 *   hw_pin 本体（L2 档案 / L1 双模驱动 / L0 电压 / L3 协议插件）
 *   只认 BSP 回调，从来不知道自己跑在哪 —— 默认走确定性模拟器
 *   （GPIO 电平状态机 + W25Q / AN3155 器件模型），
 *   真机固件只要 hw_pin_bsp_install() 注入 GPIO/UART 回调即可全链生效。
 *
 * ┌──────────────────────────────────────────────────────────────────────┐
 * │ 档案 "s3-9p"（换目标只改这一张表）                                     │
 * ├──────────┬──────┬───────────────────────────────────────────────────┤
 * │ 逻辑信号  │ GPIO │ 说明                                               │
 * ├──────────┼──────┼───────────────────────────────────────────────────┤
 * │ MOSI     │ 11   │ SPI 主出从入（bit-bang 出 / SPI2 FSPID 出）          │
 * │ MISO     │ 13   │ SPI 主入从出（bit-bang 读 / SPI2 FSPIQ 入）          │
 * │ CK       │ 12   │ SPI 时钟                                           │
 * │ CS       │ 14   │ 片选（低有效）                                       │
 * │ TX       │ 18   │ UART1 TX（ISP 发）                                  │
 * │ RX       │ 8    │ UART1 RX（ISP 收）                                  │
 * │ RST      │ 9    │ 目标复位 / 进编程模式                                │
 * │ VPP      │ 21   │ 编程电压（LEDC PWM → RC → 升压；ADC1_CH3 同脚回读）  │
 * │ VCC      │ 47   │ 目标供电（本版只驱动电平，真 VCC 需外部 MOSFET）      │
 * └──────────┴──────┴───────────────────────────────────────────────────┘
 *
 * ⚠️ S3 选脚约束（硬件事实，勿改）：
 *   GPIO 19/20  = 原生 USB-JTAG，占用
 *   GPIO 26~32  = SPI0/1 片内 Flash，占用
 *   GPIO 33~37  = 八线 PSRAM（本板 CONFIG_SPIRAM_MODE_OCT=y），占用
 *   GPIO 43/44  = UART0 调试口
 *   GPIO 0/3/45/46 = strapping，避开
 *   产品固件（esp32-s3-lvgl-ui）还会占 LCD 4/5/6/7/10 与按键 15/16/17
 *   → 本测试固件不跑 UI，但选脚仍按「不与产品固件抢脚」保守挑。
 *
 * ⚠️⚠️ 为什么必须有「档案」这一层（本轮实测结论）
 *   hw_pin 内置的 esp32-9p / w25q 档案写的是 **ESP32-classic** 脚号
 *   （MOSI=23 / MISO=19 / CK=18 / CS=5）。这些脚号在 **ESP32-S3 上不存在**
 *   （S3 只有 0..21 与 26..48）。
 *   ⇒ 「换目标芯片 = 换一条档案」不是口号，是**上真机的必要动作**：
 *     真机固件用 hw_pin_profile_load(&s_prof_s3) 换档案，模块代码一行不动。
 *
 * ⚠️ 自证策略（本层最关键的取舍，与 hw_dc 同宗）
 *   · 引脚面（GPIO 内部上下拉 / 模块写→硬件读）→ **不需任何外部器件**（硬证据）
 *   · SPI 面（bit-bang / HW 两模）→ 需一根杜邦线 MOSI(IO11) ↔ MISO(IO13)。
 *     为什么不能「本脚自驱自测」：MISO 脚在档案里是**输入**，
 *     没接器件时读到的是悬空/弱上拉电平，与 MOSI 驱动电平的分离度
 *     完全来自线容 + 内部 45kΩ 上拉的 RC，处于「临界摆动」区间
 *     （τ ≈ 45k × 20pF ≈ 0.9µs，而 bit-bang 半周期约 1µs）——
 *     拿这种读数当「SPI 通了」就是**空转假通过**。
 *     接上跳线后对端是**推挽驱动**，高低电平干净利落 → 这才是真证据。
 *     无跳线只报 SKIP，绝不用噪声冒充通过（hw_dc 已用对照组抓到过同款假阳性）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hw_pin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 板级引脚（换线只改这一段）
 * ============================================================ */
/* --- 模块档案内的 9 个信号 --- */
#define PIN_S3_MOSI      11
#define PIN_S3_MISO      13
#define PIN_S3_CK        12
#define PIN_S3_CS        14
#define PIN_S3_TX        18
#define PIN_S3_RX        8
#define PIN_S3_RST       9
#define PIN_S3_VPP       21
#define PIN_S3_VCC       47

/* --- 引脚自证专用脚（**不在档案里**，只在 BSP 层用；空闲脚，无板载外设）--- */
#define PIN_S3_PROOF_A   41
#define PIN_S3_PROOF_B   42

/* --- 编程电压：PWM 频率/分辨率 + ADC 通道 --- */
#define PIN_VPP_PWM_HZ   20000
#define PIN_VPP_PWM_BITS 10          /* 0..1023 */
#define PIN_VPP_ADC_CH   3           /* GPIO21 = ADC1_CH3 (S3 映射: ch = pin-1) */

/* ============================================================
 * 生命周期
 * ============================================================ */
int  pin_hw_hw_init(void);
void pin_hw_hw_deinit(void);

/* UART1 = BSP 的 uart_putc/uart_getc 载体（也是 ISP 通道）。 */
#define PIN_UART_PORT    1

/* 换档案（把 s3-9p 装载进 hw_pin 模块）。返回 0 / 负 = 失败。 */
int  pin_hw_profile_load(void);

/* 装载仅 UART 三线的 ISP 档案 "s3-isp"（TX/RX/RST，SPI 四线未映射）。 */
int  pin_hw_prof_isp_load(void);

/* 切换 L1 双模驱动的档位：HW_PIN_DRV_BB / HW_PIN_DRV_HW（同一档案，只换驱动）。 */
void pin_hw_drv_set(int drv);

/* 装载真机 BSP（real=true）或卸载回模拟（real=false）。
 *   ⚠️ 这个 NULL 就是 SIM/REAL 的分水岭。 */
int  pin_hw_install(bool real);

/* ============================================================
 * 真实性自证原语
 * ============================================================ */

/* ① GPIO 内部上下拉自证（**不需要任何外部器件**，硬证据）：
 *    两个空闲脚各「下拉→读 / 上拉→读」，4 个期望值全中才返回 0。
 *    空转必失败 —— 伪造必须真的让引脚电平跟着配置跳变。 */
int  pin_hw_prove_gpio(int *pd_a, int *pu_a, int *pd_b, int *pu_b);

/* ② 档案脚「模块写 → 硬件读」闭环（**不需要任何外部器件**，硬证据）：
 *    用 hw_pin_gpio_write 往档案内的空闲脚（RST/CS）写 1/0，再 gpio_get_level 直读回。
 *    证明「模块写的引脚真的是这颗芯片的这颗脚」。 */
int  pin_hw_prove_owngpio(int *hi, int *lo);

/* ③ bit-bang SPI 环回（**需要一根跳线** MOSI(IO11) ↔ MISO(IO13)）：
 *    走模块自己的 bit-bang 路径（L1 双模驱动的 BB 档）发已知模式 → 读回。
 *      返回 0 = 跳线在位且逐字节一致 → **真实 bit-bang SPI 烧录链路成立**（硬证据）
 *      返回 1 = 未接跳线（SKIP，不算失败）
 *      返回 <0 = 出错 */
int  pin_hw_spi_bb_loopback(const uint8_t *tx, uint8_t *rx, int n, int *nrx);

/* ④ 硬件 SPI（SPI2/FSPI）环回（**同一根跳线** MOSI(IO11) ↔ MISO(IO13)）：
 *    L1 双模驱动的 HW 档 = 外设映射（GPIO Matrix），与 BB 档是两条独立通路。
 *      返回 0 = 在位且一致；1 = 无跳线（SKIP）；<0 = 出错 */
int  pin_hw_spi_hw_loopback(const uint8_t *tx, uint8_t *rx, int n, int *nrx);

/* ============================================================
 * 真实异步链路
 * ============================================================ */
/* UART1 内部回环（uart_set_loop_back）：只证「UART 外设本身收发逐位一致」。 */
int  pin_hw_uart_loopback_open(void);
void pin_hw_uart_loopback_close(void);
int  pin_hw_uart_loopback_roundtrip(const uint8_t *out, int n,
                                    uint8_t *in, int cap, int timeout_ms);

/* UART1 走线模式（**真链路**）：TX 驱动 IO18、RX 读 IO8 —— 与目标器件相接。
 *   ⚠️ 必须在装 BSP 之前调用（UART1 就是 BSP 的 uart_putc/uart_getc 载体）。 */
int  pin_hw_uart_link_open(void);
int  pin_hw_uart_link_opened(void);   /* 只读状态查询：走线链路是否已就绪 */
void pin_hw_uart_link_close(void);

/* 导线自证（**需要一根跳线** TX(IO18) ↔ RX(IO8)）：
 *   走线模式下自发自收 4 字节 → 证明「真实导线通路」成立。
 *   ⚠️ 与「UART1↔UART2 器件链路」互斥：若目标器件同时在 IO8 上驱动，
 *      两个推挽输出会同网打架。做器件链路测试前请拆掉这根跳线。 */
int  pin_hw_uart_wire_probe(const uint8_t* out, int n, uint8_t* in, int cap);

/* ============================================================
 * L0 编程电压层
 * ============================================================ */
int  pin_hw_vpp_pwm_set(int duty);
int  pin_hw_vpp_pwm_max(void);

/* 重新把 LEDC 信号接到 VPP 脚上（现场取证用）：
 *   hw_dc 已实测「adc_oneshot_config_channel 会独占 pad 并关掉数字输出驱动器」，
 *   这里验证它**是否同样关掉 LEDC 外设输出** —— 先直读、再重挂 LEDC 后重读。 */
void pin_hw_vpp_reassert(void);

/* 回读 VPP 引脚：ADC 平均 mV + 该脚数字电平。
 *   无升压硬件时 PWM 经 RC 就是 0..3.3V 的可变电压 —— 本层**如实回读**，
 *   不谎报 12V。返回 0 / -1。 */
int  pin_hw_vpp_probe(int *mv, int *level);

/* 引脚体检（仅 -DPIN_PIN_SCAN=1 编译）。 */
#if PIN_PIN_SCAN
int  pin_hw_scan(void);
#endif

/* 诊断打印 */
void pin_hw_dump(void);

#ifdef __cplusplus
}
#endif
