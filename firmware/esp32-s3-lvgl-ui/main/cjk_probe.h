/**
 * cjk_probe.h — 中文字体「端到端可上屏」验证探针
 *
 * 见 cjk_probe.c 顶部说明。要点：
 *   · CJK_SMOKETEST=0（产品构建）下两个函数都是空实现，零开销。
 *   · 探针代码只住在 cjk_probe.c，不往 ui.c / app_main.c 塞测试逻辑。
 *
 * 用法：
 *   idf.py -DCJK_SMOKETEST=1 -DSHOT_PROBE=1 \
 *          -DSHOT_PAGE_FIRST=99 -DSHOT_PAGE_LAST=99 build flash
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 字形覆盖自检。遍历 174 条文案 × 2 语言，逐一问 LVGL 字体里有没有该码位，
 * 另抽查 6 个「文案表之外、GB2312 全量之内」的字。
 * 打印 [PASS]/[FAIL] 行；不依赖屏幕，可在刷机前的任何环境跑。
 *
 * ⚠️ PASS 只代表「字在字体里」，不代表「上了屏」。上屏判据是抓屏 OCR。
 */
void cjk_probe_report(void);

/**
 * 构建并加载中文字体冒烟屏（独立 lv_obj_screen，不进 ui.c 的 10 页）。
 * 必须在 lcd_hw_init() 之后、lcd_hw_start() 之前调用（LVGL 独占期）。
 */
void cjk_probe_init(void);

#ifdef __cplusplus
}
#endif
