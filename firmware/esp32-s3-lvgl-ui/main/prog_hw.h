/**
 * prog_hw.h — 烧录器真机 BSP（ESP32-S3）
 *
 * 这一层是 hw_pin 知识页里记的那个「唯一缺口」：
 *   hw_pin 本体（引脚档案 + 双模驱动 + 25xx/ISP 协议）只认 BSP 回调，
 *   从来不知道自己跑在哪 —— 默认走确定性模拟器（W25Q 器件模型），
 *   真机固件只要 hw_pin_bsp_install() 注入 GPIO 回调即可全链生效。
 *
 * 设计要点（都是踩过坑换来的，别随手改）：
 *   ① **不动 hw_pin 的内置黄金表**：本板档案用 hw_pin_profile_load() 注入，
 *      这样 HW_PIN_GOLDEN 校验（内置 4 条档案的 FNV）仍成立，回归不破。
 *   ② **SIM / REAL 的开关就是那个 NULL**：hw_pin 的 bit-bang 里
 *      use_dev = (g_bsp.gpio_read == NULL) ——
 *        SIM  → BSP 全 NULL → 器件模型供 MISO → RDID 稳定 EF 40 18；
 *        REAL → 装了 gpio_read → 真读 MISO 引脚 → 没接芯片就该诚实报 NOFLASH。
 *      这是「不假装成功」的结构性保证，不靠上层自觉。
 *   ③ **方向要自己配**：hw_pin 从不调用 bsp->gpio_dir（grep 实测零命中），
 *      所以 MOSI/CK/CS 必须先配成输出、MISO 配成输入，否则 GPIO 默认处于
 *      复位态（部分脚带 pull-up/down 或 floadting），bit-bang 直接失真。
 *   ④ **UART 走 GPIO Matrix**：ESP32-S3 的 uart_set_pin() 允许把 TX/RX
 *      路由到任意空闲脚 —— 这是「用 GPIO 定义烧录器」在 ESP32 上一等公民的体现。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hw_pin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 三条板级档案（换目标 = 换一条档案；本板脚位见 prog_hw.c 注释）---- */
const hw_pin_profile_t *prog_hw_profile_spi(void);  /* s3-spi : SPI 四线 + UART 两线（全功能） */
const hw_pin_profile_t *prog_hw_profile_w25q(void); /* w25q   : 纯 SPI 四线 */
const hw_pin_profile_t *prog_hw_profile_isp(void);  /* s3-isp : 纯 UART 三线（TX/RX/RST） */

/* 按名字取档案；找不到返回 NULL */
const hw_pin_profile_t *prog_hw_profile_by_name(const char *name);

/* 装载档案 + 选驱动模式。
 *   real=false → hw_pin 走确定性模拟器（W25Q 器件模型）
 *   real=true  → 注入真 GPIO BSP + 配好引脚方向 + 开 UART
 * 返回 HW_PIN_R_OK / HW_PIN_R_NODEV / HW_PIN_R_BADARG */
int prog_hw_install(const hw_pin_profile_t *p, bool real);

/* 真机自证（不需要外部芯片）：把 MOSI 与 MISO 用一根跳线短接，
 * 则 bit-bang 发出去的每一位都会被同时读回 —— 发什么读什么。
 * 这是「引脚档案 + GPIO 驱动 + 时序」三件事唯一不依赖目标器件的硬证据。
 * 返回 0 = 全对；>0 = 首个出错字节下标；<0 = 档案里没有 SPI 脚，测不了。 */
int prog_hw_loopback(uint8_t *tx_out, uint8_t *rx_out, uint32_t *n_out);

/* 把当前档案的引脚映射打到串口（诊断；REAL 模式会标出实际 GPIO 号） */
void prog_hw_dump(void);

/* 释放本层（回 SIM / 关 UART） */
void prog_hw_release(void);

#ifdef __cplusplus
}
#endif
