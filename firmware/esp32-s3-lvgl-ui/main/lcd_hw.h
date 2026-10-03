/**
 * lcd_hw.h — 2.0" ST7789P3 (240x320, 10 脚) 硬件层 + LVGL 对接
 *
 * 接线（与你已验证的 panel_lcd 保持一致）：
 *   MOSI/SDA = GPIO7   SCLK/SCL = GPIO6   DC/RS = GPIO4   RST = GPIO5   CS = GPIO10
 *   背光 LED+ -> 3V3(串限流) | LED- -> GND   （纯电源，固件不控）
 *
 * 初始化序列 = 移植自已实测锁定的 panel_lcd.c INIT_240x320（golden 0xA36B04D1），
 * 补上 IDF 内置 esp_lcd_panel_st7789 没做的 PORCTRL/gamma/INVON 等条目。
 */
#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES   320     /* 横屏宽 */
#define LCD_V_RES   240     /* 横屏高 */

/* 屏幕接线（改屏只改这里） */
#define LCD_PIN_MOSI  7
#define LCD_PIN_SCLK  6
#define LCD_PIN_DC    4
#define LCD_PIN_RST   5
#define LCD_PIN_CS    10
#define LCD_SPI_HZ    (20 * 1000 * 1000)   /* 杜邦线留余量；排线可提到 40M */

/**
 * 初始化 SPI + ST7789 + LVGL（含 flush 回调绑定）。
 * 成功后 LVGL 可直接使用，显示尺寸 320x240 横屏。
 *
 * ⚠️ 本函数【不】启动 LVGL 刷新任务，只把硬件/LVGL 显示层准备好。
 *    调用方必须在**所有 UI 对象构建完成之后**再调用 lcd_hw_start()，
 *    否则刷新任务会和 ui_init() 并发遍历同一棵对象树 → 对象图撕裂 → 崩溃。
 */
esp_err_t lcd_hw_init(void);

/**
 * 启动 LVGL 刷新任务（lv_timer_handler 循环）。
 *
 * 必须在 lcd_hw_init() 与 ui_init() 都完成之后调用。幂等。
 * 这是 LVGL 线程安全铁律的落地点：LVGL 非线程安全，任何时刻只能有一个
 * 任务在调用 lv_*；刷新任务一旦跑起来就不停遍历对象树，所以它必须最后启动。
 */
esp_err_t lcd_hw_start(void);

/* 竞态排查探针：返回刷新任务累计调用 lv_timer_handler 的次数。
 * 用于证明「刷新任务确实与 ui_init 并发跑过」——否则不能说竞态被证伪。 */
uint32_t lcd_hw_refresh_iters(void);

/* LVGL 心跳由本模块用 esp_timer_get_time 提供，无需额外定时器 */

#ifdef __cplusplus
}
#endif
