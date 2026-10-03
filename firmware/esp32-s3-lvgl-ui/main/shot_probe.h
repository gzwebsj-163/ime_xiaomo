/**
 * shot_probe.h — 屏幕渲染 dump 探针（仅排查构建，产品构建零开销）
 *
 * 用法：
 *   idf.py -DSHOT_PROBE=1 build && idf.py -p <PORT> flash
 *   然后跑 tools/shot_fetch.py 抓串口 → 还原成 PNG
 *
 * ⚠️ 必须在 lcd_hw_start() **之前**调用（此刻 LVGL 独占，注册定时器无并发风险）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 注册一次性定时器；真正的抓图发生在 lvgl 任务的 lv_timer_handler 里。 */
void shot_probe_start(void);

#ifdef __cplusplus
}
#endif
