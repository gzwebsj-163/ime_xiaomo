/**
 * dc_hw.h — hw_dc 真机 BSP（ESP32-S3）：真实 ADC / GPIO 信号面
 *
 * 这一层是 hw_dc 的「从表驱动到真实硅」那一跳：
 *   hw_dc 本体（信号表 / 极值窗口判决 / 参考预值 / 组合数据帧）只认 BSP 回调，
 *   从来不知道自己跑在哪 —— 默认走确定性模拟器（表驱动），
 *   真机固件只要 hw_dc_bsp_install() 注入 ADC/GPIO 回调即可全链生效。
 *
 * ┌─────────┬──────┬──────────────────────────────────────────────┐
 * │ 信号     │ GPIO │ 来源                                          │
 * ├─────────┼──────┼──────────────────────────────────────────────┤
 * │ INPUT   │ 3    │ ADC1_CH2 → 校准后 mV（输入侧分压节点）          │
 * │ OUTPUT  │ 1    │ ADC1_CH0 → 校准后 mV（输出侧分压节点）          │
 * │ MAXIN   │ –    │ BSP 运行期历程：INPUT 的最大值                  │
 * │ MININ   │ –    │ BSP 运行期历程：INPUT 的最小值                  │
 * │ MAXOUT  │ –    │ BSP 运行期历程：OUTPUT 的最大值                 │
 * │ MINOUT  │ –    │ BSP 运行期历程：OUTPUT 的最小值                 │
 * │ HILITE  │ 21   │ GPIO 数字输入（比较器/中继/CHRG 类开漏信号）     │
 * │ LOLITE  │ 38   │ GPIO 数字输入（同上，默认上拉）                 │
 * └─────────┴──────┴──────────────────────────────────────────────┘
 * 回环探针脚：PROBE = 39（用跳线短接到 ADC_IN 可做 ADC 硬自证）
 *
 * ⚠️ 极值语义说明（重要）：
 *   MAXIN/MININ/MAXOUT/MINOUT 在本层是 **BSP 侧的运行期历程量**，
 *   不是硬件寄存器 —— 每次 read() 实时更新。这与「换目标芯片=换一条档案」
 *   同哲学：硬件有什么就映射什么，没有的由 BSP 按语义合成。
 *
 * ⚠️⚠️ 本板 ADC 自证的两个实测结论（2026-10-01，别踩回头路）：
 *
 *   ① **弱内部上下拉在 ADC 脚上不生效** —— `gpio_set_pull_mode(pin, PULLUP/DOWN)`
 *      后 ADC 三态读数几乎相同（实测 25/24/25 mV）。三态法判死。
 *
 *   ② **`adc_oneshot_config_channel()` 会独占 pad 并关掉数字输出驱动器** ——
 *      实测（唯一变量 = 该脚是否被配过 ADC 通道）：
 *          IO21/IO38（纯数字脚，未配置）      → 驱动回读 1/0 ✓
 *          IO2      （ADC1_CH1，未配置）      → 驱动回读 1/0 ✓
 *          IO1      （ADC1_CH0，已配置）      → 驱动回读 0/0 ✗
 *          IO3      （ADC1_CH2，已配置）      → 驱动回读 0/0 ✗
 *      ⇒ **「用本脚输出驱动来自测 ADC」在结构上不可能成立**。
 *        早期看到的「驱动 hi/lo 分离 ~400mV」是 **ADC 采样噪声漂移的假阳性**
 *        （差点当成证据，靠对照组才抓出来 —— 典型的「空转假通过」）。
 *      ⇒ ADC 的硬证据**必须**用一根**非 ADC 脚**去驱动、再用 ADC 读，
 *        即需要一根跳线（与 hw_pin 需 MOSI↔MISO 跳线同源）。
 *        无跳线时我们只报 SKIP，**拒绝用噪声漂移冒充读数正确性**。
 *
 * ⚠️ 引脚是**实测**挑的，不是猜的：跑 `-DDC_PIN_SCAN=1` 的引脚体检，
 *   结论 = ADC_IN(IO3) / ADC_OUT(IO1) 分离度最高。
 *   注意 LCD 脚 IO4/5/6/7/10 被屏模块外拉钉住（sep≈0，完全无响应）→ 不可用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hw_dc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 板级引脚（换线只改这一段；⚠️ 换板须先跑 -DDC_PIN_SCAN=1 体检）---- */
#define DC_PIN_ADC_IN    3     /* ADC1_CH2，输入侧 */
#define DC_PIN_ADC_OUT   1     /* ADC1_CH0，输出侧 */
#define DC_PIN_HILITE    21    /* 数字输入，默认下拉 */
#define DC_PIN_LOLITE    38    /* 数字输入，默认上拉 */
#define DC_PIN_PROBE     39    /* 数字输出，回环探针（短接到 ADC_IN 做硬自证）*/

/* ---- ADC 原始值 → mV 的兜底换算（无校准方案时用）---- */
#define DC_ADC_RAW_MAX   4095

/* ---- 装载 / 释放 ---- */
int  dc_hw_hw_init(void);
void dc_hw_hw_deinit(void);

/* 装载真机 BSP（real=true）或卸载回模拟（real=false）。
 *   ⚠️ 这个 NULL 就是 SIM/REAL 的分水岭：hw_dc 的 read 装了就走真机。 */
int  dc_hw_install(bool real);

/* ---- 供验证桥使用的直读接口（绕开模块，拿"本底"值做对拍锚点）---- */
int  dc_hw_adc_mv(uint8_t side);
int  dc_hw_adc_raw(uint8_t side);

/* ---- 真实性自证原语 ---- */

/* ① 数字通路自证（**不需要任何外部器件**，硬证据）：
 *    靠内部上拉/下拉让 HILITE / LOLITE 各跳变一次，4 个期望值全中才返回 0。
 *    空转必失败 —— 想伪造必须把引脚真的拉起来。 */
int  dc_hw_prove_digital(int *pd_hi, int *pu_hi, int *pu_lo, int *pd_lo);

/* ② ADC 存活体检（软证据）：连采 16 次，要求全部落在 [0,3300] mV。
 *    只证明「ADC 单元在采样」，**不**证明读数正确（正确性靠跳线硬证）。
 *    返回 0 = 存活；<0 = 异常。vmin/vmax/vavg 可空。 */
int  dc_hw_adc_sane(int *vmin, int *vmax, int *vavg);

/* ③ ADC 硬自证（**需要一根跳线** PROBE ↔ ADC_IN）：
 *    PROBE 是纯数字脚（未被 ADC 占用 → 驱动有效），驱动高/低各读一次 ADC。
 *      返回 0  = 跳线在位且分离度足够 → ADC 读数正确性成立（硬证据）
 *      返回 1  = 未接跳线（SKIP，不算失败）
 *      返回 <0 = 出错 */
int  dc_hw_probe_loopback(int *mv_high, int *mv_low);

/* ---- Phase C: DCPP 帧层的真机传输（内部 UART 回环，无需任何外部器件）----
 *
 * 为什么需要这一层：
 *   信号层证明的是「引脚真的能读」；DCPP 证明的是「帧真的能走线」。
 *   两者是不同的证据 —— 帧的状态机（feed 逐字节）在宿主内存里过了，
 *   但不代表字节经过**真实 UART 外设**（FIFO / 波特率 / 采样）后仍逐位一致。
 *   本节用 UART1 内部回环（uart_set_loop_back）把这条路径补上。
 *
 * 引脚：TX=17 / RX=18（均为空闲脚，避开 ADC 1/3、dc_hw 21/38/39、LCD 4/5/6/7/10、
 *       PSRAM 33~37）。内部回环下这两个 GPIO 不接外部器件也能自证。
 */
#define DC_UART_PORT   1
#define DC_UART_TX     17
#define DC_UART_RX     18

/* 打开 UART1（115200 8N1 + 内部回环）。返回 0 / -1。 */
int  dc_hw_uart_open(void);
/* 关闭并释放 UART1。 */
void dc_hw_uart_close(void);

/* 把 out[0..n) 写进 UART → 回环读回。返回读到的字节数；<0 = 出错。
 *   timeout_ms: 等待第一个字节的超时；收齐 n 字节的读窗口会自适应放宽。 */
int  dc_hw_uart_roundtrip(const uint8_t* out, int n,
                          uint8_t* in, int cap, int timeout_ms);

/* 打印当前真实读数（诊断） */
void dc_hw_dump(void);

/* ---- 板级 bring-up（仅 -DDC_PIN_SCAN=1 编译；换板排查用）---- */
#if DC_PIN_SCAN
/* ADC 脚体检：GPIO1..10 逐个「驱动 hi/lo → 读 ADC」，报告分离度，
 * 返回分离度最大的脚（<0 全失败）。用于实测挑选干净 ADC 脚。 */
int  dc_hw_scan_adc(void);
/* 驱动+回读对照：判定平台是否允许「本脚自驱动自测」（结论见文件头 ②）*/
void dc_hw_drive_readback(void);
#endif

#ifdef __cplusplus
}
#endif
