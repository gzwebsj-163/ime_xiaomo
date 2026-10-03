/**
 * app_main.c — 入口
 *
 * ⚠️ 顺序是硬约束，不能调换：
 *    1. lcd_hw_init()  建 SPI→panel_io→panel→LVGL display（不含刷新任务）
 *    2. ui_init()      建全部 UI 对象（此时无并发，安全）
 *    3. lcd_hw_start() 最后才启动 LVGL 刷新任务
 *
 * 关于这个顺序（2026-09-29 受控实验后的定论，勿再照旧说辞）：
 *   · 曾有假设：崩溃=「刷新任务 vs ui_init 竞态」。
 *     → 用 RACE_AMPLIFY 放大到「必然并发」后 14/14 不崩，**该假设被证伪**。
 *   · 真正根因：LVGL 默认内置 TLSF 堆只有 64KB，UI 构建期分配失败 →
 *     lv_malloc 返回 NULL 而 LVGL 默认不检查 → 拿 NULL/野指针当合法地址写。
 *     修复 = CONFIG_LV_USE_CLIB_MALLOC=y (+ ASSERT_MALLOC 兜底)，见 sdkconfig.defaults。
 *   · 拆时序（3 必须最后）依然保留：LVGL 非线程安全，
 *     「刷新任务与 ui_init 并发」本身就是 UB，属正确的防御性写法，但非根因。
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#include "lcd_hw.h"
#include "ui.h"
#include "wifi_sta.h"    /* 真实 WiFi STA（2026-09-29 新增） */
#include "ui_cfg.h"      /* P0 NVS 配置层（2026-10-01） */
#include "i18n.h"        /* g_ui_lang */
#if PIN_PROBE
#include "pin_probe.h"
#endif
#if SHOT_PROBE
#include "shot_probe.h"
#endif
#if PROG_UI_TEST
/* 烧录页「按键 → 后端」写路径自检（仅排查构建） */
void prog_ui_probe_start(void);
#endif
/* 中文字体上屏验证探针（仅排查构建）。产品构建下这两个是空实现，
   免得 app_main 里堆 #if —— 探针细节不该漏进产品入口。 */
#include "cjk_probe.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "=== AI 远程调试器 UI (LVGL) 启动 ===");
    cjk_probe_report();        /* 刷机前就能跑：字形覆盖自检，不依赖屏幕 */

    /* ---- P0 判据②「断电重启设置不丢」：NVS 配置层 ----
     * ⚠️ 必须排在 ui_init() 之前：语言要赶在 UI 第一次取文案之前灌进
     *    g_ui_lang，否则首帧先按中文建好、之后才被改成英文，等于白屏一帧错语。 */
    ui_cfg_t cfg;
    bool cfg_loaded = ui_cfg_init(&cfg);
    g_ui_lang = cfg.lang;      /* 出厂默认中文（cfg_loaded=false 时也是 0） */
    {
        int64_t now = ui_cfg_now();
        ESP_LOGI(TAG, "配置: %s lang=%u(%s) sntp=%u 时间=%s",
                 cfg_loaded ? "已载入" : "首启/无记录",
                 cfg.lang, UI_LANG_NAME[cfg.lang], cfg.sntp,
                 (now > 0) ? "已设定" : "未定(待首启向导)");
    }

#if UI_CFG_SELFTEST
    {
        /* 仅排查构建：写入回读逐位对拍 4 项，跑完自动还原原配置 */
        int f = ui_cfg_selftest();
        ESP_LOGW(TAG, "UI_CFG_SELFTEST %s (fails=%d)", (f == 0) ? "PASS" : "FAIL", f);
    }
#endif

#if UI_CFG_PERSIST_TEST
    /* 仅排查构建：必须跑两轮。放在 selftest 之后 —— selftest 会擦 NVS 再还原，
     * 若反序，写进去的配置会被 selftest 的还原动作覆盖掉。 */
    {
        int p = ui_cfg_persist_probe();
        ESP_LOGW(TAG, "UI_CFG_PERSIST_TEST %s", (p == 0) ? "OK" : "FAIL");
    }
#endif

#if PIN_PROBE
    pin_probe_run();     /* 仅排查构建：按键引脚可用性 + PSRAM 完整性对拍 */
#endif

    esp_err_t err = lcd_hw_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_hw_init 失败: %s —— UI 不启动", esp_err_to_name(err));
        return;
    }

#if RACE_AMPLIFY
    /* ⚠️ 反面教材（仅排查构建）：故意用「旧顺序」——
     *    刷新任务先跑起来，然后才建 UI 对象，且在每次 lv_obj_create 后让出 CPU。
     *    用于对「刷新任务 vs ui_init 竞态」假设做**因果实证**：
     *    若此构建稳定崩溃 → 机制成立，修复（拆时序）有据。
     */
    ESP_LOGW(TAG, "!!! RACE_AMPLIFY 已启用：旧顺序 + ui_init 让出点（仅排查用）!!!");
    err = lcd_hw_start();            /* 旧顺序：刷新任务先启动 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_hw_start 失败: %s", esp_err_to_name(err));
        return;
    }
    ui_init();                       /* 与刷新任务并发遍历同一棵对象树 */
    {
        /* 证据 ①：并发是否真的发生 —— 刷新任务在 ui_init 期间跑了多少轮？
         * 证据 ②：app_main 栈水位 —— 排掉「栈溢出」这条线 */
        uint32_t iters = lcd_hw_refresh_iters();
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "RACE_AMPLIFY 结果: ui_init 期间刷新任务已跑 %u 轮"
                      "（>100 即确证并发）| app_main 栈剩余 %u 字(%u 字节)",
                 (unsigned)iters, (unsigned)hw, (unsigned)(hw * sizeof(StackType_t)));
        ESP_LOGW(TAG, "RACE_AMPLIFY: ui_init 竟然跑完了（未崩溃）");
    }
#else
    ui_init();                       /* ① 建对象：此刻刷新任务还没跑，独占 LVGL */

    /* 首启（NVS 里没有合法配置）→ 直接进向导；否则停在开机页，2.4s 后进菜单。
     * ⚠️ 时序：ui_init() 已建好全部页面，这里只是切当前页，无并发风险。 */
    if (!cfg_loaded) {
        ESP_LOGI(TAG, "首次使用：进入设置向导（语言 → 时间 → 进入主菜单）");
        ui_start_wizard();
    }

    cjk_probe_init();                /* 冒烟屏覆盖到屏上（产品构建=空实现） */

    /* 真实 WiFi STA：放在 ui_init 之后启动 —— 先让 UI 把内部 RAM 占稳，
     * 再让 WiFi 驱动去申请它那几十 KB 缓冲，避免反过来把 UI 挤到 OOM
     * （UI 分配失败曾经以 StoreProhibited 崩溃循环的形式出现过）。
     * 非阻塞：连接/扫描全在 wifi_mgr 任务里跑，不拖慢启动。 */
    wifi_st_start();

#if SHOT_PROBE
    /* 仅排查构建：注册一次性定时器。此刻 LVGL 仍独占 → 注册无并发风险；
     * 真正的抓图发生在 lvgl 任务的 lv_timer_handler 里（安全上下文）。 */
    shot_probe_start();
#endif

    err = lcd_hw_start();            /* ② 对象建完，才放刷新任务出来 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_hw_start 失败: %s", esp_err_to_name(err));
        return;
    }

#if PROG_UI_TEST
    /* 仅排查构建：刷新任务已在跑 → 此时注入合成按键才有人消费。
     * 任务自己会先睡一会儿再开始，不抢 boot 日志。 */
    prog_ui_probe_start();
#endif
#endif

    ESP_LOGI(TAG, "UI 就绪：v3 九页（开机 / 向导 / 菜单 / 探针 / 烧录 / 引脚 / 状态 / 设置 / 关于）");
}
