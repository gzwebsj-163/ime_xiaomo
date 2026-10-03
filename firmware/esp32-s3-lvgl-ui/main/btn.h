/**
 * btn.h — 三个物理按键（上 / 下 / 返回）
 *
 * 接线方案（推荐，抗干扰最好）：
 *   ┌──────────┬──────────┬──────────────┬─────────────┐
 *   │ 功能     │ ESP32-S3 │ 按键另一端   │ 上拉电阻    │
 *   ├──────────┼──────────┼──────────────┼─────────────┤
 *   │ 上 UP    │ GPIO38   │ GND          │ 10kΩ → 3V3  │
 *   │ 下 DOWN  │ GPIO39   │ GND          │ 10kΩ → 3V3  │
 *   │ 返回 BACK│ GPIO17   │ GND          │ 10kΩ → 3V3  │
 *   └──────────┴──────────┴──────────────┴─────────────┘
 *
 * 🚨 2026-09-29 实测结论：GPIO35/GPIO36 **不可用**（曾按要求改到 35/36，已回退）
 *   本板 CONFIG_SPIRAM_MODE_OCT=y，八线 PSRAM 固定占用 GPIO33~GPIO37
 *   （GPIO35=SPIIO6、GPIO36=SPIIO7，硬连到 PSRAM 芯片数据线）。
 *   `pin_probe.c` 真机 A/B 对照实测（唯一变量=引脚）：
 *     · gpio_config(35,36, 输入+上拉) 返回 ESP_OK   ← IDF 不拦，属静默通过
 *     · 但配置之后 PSRAM 的**密集访问立即卡死**（4KB 密写从 3084us 变成永不返回）
 *       → 5 次 TG1WDT 复位；同一份代码换到空闲脚 IO18 则 0 次复位、全程正常。
 *     · 且"按键按下=把 PSRAM 数据线对地短路"，与 PSRAM 输出驱动对抗 → 数据错/崩溃风险。
 *   ⇒ 换脚请只用空闲脚：15/16/17/18/8/9/11/12/13/14/21/38~42（见下方「选脚理由」）。
 *   可选：每个脚对 GND 并 100nF 电容做硬件去抖（软件已有 60ms 去抖）
 *
 * 低电平有效：按下 = 0，松开 = 1。
 * 固件已开内部上拉（~45kΩ）作为「没接外部上拉也能工作」的兜底；
 * 但长引线建议加外部 10kΩ，内部上拉的阻值偏大抗噪差。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 引脚定义（换脚只改这三行）-----
 * 选脚理由：避开
 *   GPIO0/3/45/46   strapping 脚（上电电平决定启动模式）
 *   GPIO19/20       USB-Serial-JTAG D+/D-
 *   GPIO26~32       SPI0/1 Flash
 *   GPIO33~37       八线 PSRAM（本板 8MB）
 *   GPIO43/44       UART0 控制台
 *   GPIO4/5/6/7/10  已给 LCD
 */
#define BTN_PIN_UP      38
#define BTN_PIN_DOWN    39
#define BTN_PIN_BACK    17

/* 按下时的电平：0 = 低电平有效（按键接 GND） */
#define BTN_ACTIVE_LEVEL    0

/* 长按判定阈值（按住多久算长按）。
 * 实机手感：1000~1500ms 合适；改这一行即可，解码器会跟着走。 */
#define BTN_LONG_PRESS_MS   1200

/* ---------- 去抖参数（真机实测定版，详见 btn_decode.h）----------
 *   按下确认  30ms   —— 跟手，不拖泥带水
 *   抬起确认 300ms   —— 瞬断静默期：按住时的慢速瞬断当噪声吞掉
 *   真松开   500ms   —— 解除长按静默
 *   ⚠️ 副作用：同一键连按，间隔需 ≥300ms 才会计数（实测按压节拍 >400ms，够用）
 *
 * ---------- 按键语义（UI 侧，见 ui.c 的 handle_buttons）----------
 *   短按 UP / DOWN   ：同层前后切换（菜单里移光标；功能页里换页）
 *   长按 UP          ：进入下一级（菜单 → 选中项的功能页）
 *   长按 DOWN        ：返回上一级（功能页 → 菜单）
 *   单击 BACK        ：返回主菜单（任何层级）
 *   长按 BACK        ：恢复 3s 自动轮播（AUTO）
 *
 * 🚨 事件时机（2026-09-29 第二轮定版，别再改回去）：
 *   短按 = 「按下 → 抬起」完整手势闭合后才上报（抬手后约 300ms）
 *   长按 = 按住到 BTN_LONG_PRESS_MS 那一刻上报，且**只上报一次**，
 *          之后静默到真松开。⇒ 长按是「锁定触发」，不会先附带一次短按。
 */

typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_BACK,
    BTN_MAX
} btn_id_t;

/**
 * 初始化三个按键：配置上拉输入 + 启动 20ms 轮询任务。
 * 任务只负责“去抖 + 入队”，绝不碰 LVGL（LVGL 非线程安全）。
 */
esp_err_t btn_init(void);

/**
 * 取一个待处理事件（非阻塞）。
 * @param id       出参：哪个键
 * @param is_long  出参：true = 长按事件，false = 短按（按下瞬间）
 * @return true 取到事件，false 队列为空
 *
 * ⚠️ 必须从 LVGL 所在任务（如 lv_timer 回调）调用，才能安全地更新界面。
 */
bool btn_get_event(btn_id_t *id, bool *is_long);

/**
 * ⚠️ 测试钩子（仅排查用，产品逻辑从不调用）
 *
 * 把一个**合成事件**直接塞进真实的事件队列 —— 与物理按键走的是**同一条路径**
 * （环形队列 → handle_buttons → ...），因此可以脚本化地自证「按键 → 后端」这条写路径。
 * 用例见 prog_ui_probe.c（烧录页写路径）与 btn.c 的 BTN_SELFTEST（菜单导航链路）。
 *
 * 线程安全：内部走 portMUX 临界区，可从任意任务调用。
 */
void btn_inject(btn_id_t id, bool is_long);

#ifdef __cplusplus
}
#endif
