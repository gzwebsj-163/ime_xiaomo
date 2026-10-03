/**
 * btn.c — 三键去抖 + 事件队列
 *
 * 设计要点：
 *   1. 20ms 轮询 + 「连续 3 次采样一致（60ms）」才算有效跳变 → 软件去抖
 *   2. 按键任务只写环形队列，不调用任何 lv_* 接口
 *      （LVGL 不是线程安全的，跨任务直接改界面必崩）
 *   3. 队列用 portMUX 临界区保护，无动态分配、无 ISR，逻辑最短路径
 */
#include "btn.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "btn";

static const int   s_pin[BTN_MAX]  = { BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_BACK };
static const char *s_name[BTN_MAX] = { "UP", "DOWN", "BACK" };

/* ============================================================================
 * 去抖 = 交给 btn_decode.h 的纯逻辑状态机（单一实现，宿主可测）
 *
 * 🕳️ 上一版的坑：按下沿和抬起沿都用同一个 60ms 窗口 → 只挡得住毫秒级弹跳。
 *    实机实测「按住 UP 不放」会连续吐出 12 个短按事件（间隔 300~900ms）：
 *    触点慢速瞬断被当成「抬起 + 重新按下」→ 按下时刻被不断重置
 *    → 长按计时器永远凑不满阈值 → **长按功能形同虚设**。
 *    详细复盘与修法见 btn_decode.h 顶部注释。
 * ========================================================================== */
#include "btn_decode.h"

#define POLL_MS  BTN_POLL_MS     /* ⚠️ 必须等于 BTN_POLL_MS，解码器按此周期计时 */
#define EVQ_LEN  16

typedef struct {
    uint8_t id;
    uint8_t is_long;
} btn_ev_t;

/* ---- 事件环形队列 ---- */
static btn_ev_t          s_q[EVQ_LEN];
static volatile int      s_head = 0;      /* 写指针 */
static volatile int      s_tail = 0;      /* 读指针 */
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- 每键解码状态（纯逻辑，见 btn_decode.h） ---- */
static btn_dec_t s_dec[BTN_MAX];

static void push(btn_id_t id, bool is_long)
{
    portENTER_CRITICAL(&s_lock);
    int next = (s_head + 1) % EVQ_LEN;
    if (next != s_tail) {                 /* 队列满则丢弃最旧事件 */
        s_q[s_head].id = (uint8_t)id;
        s_q[s_head].is_long = is_long ? 1 : 0;
        s_head = next;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* 测试钩子：把合成事件塞进真实队列（与物理按键同路径，见 btn.h 说明与 prog_ui_probe.c） */
void btn_inject(btn_id_t id, bool is_long)
{
    push(id, is_long);
}

bool btn_get_event(btn_id_t *id, bool *is_long)
{
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (s_tail != s_head) {
        *id = (btn_id_t)s_q[s_tail].id;
        *is_long = s_q[s_tail].is_long != 0;
        s_tail = (s_tail + 1) % EVQ_LEN;
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

static void poll_one(int i)
{
    const uint8_t raw = (gpio_get_level(s_pin[i]) == BTN_ACTIVE_LEVEL) ? 1 : 0;
    const int64_t now = esp_timer_get_time();

    const int ev = btn_decode_step(&s_dec[i], raw, now, BTN_LONG_PRESS_MS);
    if (ev == BTN_EV_SHORT) {
        push((btn_id_t)i, false);
        ESP_LOGI(TAG, "%s 短按（抬起确认）", s_name[i]);
    } else if (ev == BTN_EV_LONG) {
        push((btn_id_t)i, true);
        ESP_LOGI(TAG, "%s 长按（按住 %dms 到阈值，锁定触发，之后静默直到真松开）",
                 s_name[i], BTN_LONG_PRESS_MS);
    }
}

static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < BTN_MAX; i++) poll_one(i);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

#if BTN_SELFTEST
/* ============================================================================
 * 合成事件注入自检（仅 -DBTN_SELFTEST=1 时编译，产品构建不含）
 *
 * 目的：不依赖物理按键，把「环形队列 → tick_cb → handle_buttons → ui_goto
 *       → lv_obj_set_hidden(10 页)」整条链路跑一遍，证明它不崩。
 * 覆盖：next×3 / prev×1 / back-short 两条分支 / back-long / up-long
 * ========================================================================== */
typedef struct { btn_id_t id; bool lg; int delay_ms; const char *desc; } st_step_t;

static const st_step_t s_steps[] = {
    { BTN_DOWN, false, 3000, "DOWN 单击 -> 菜单光标 0->1 (I2C)" },
    { BTN_DOWN, false, 1000, "DOWN 单击 -> 菜单光标 1->2 (SPI)" },
    { BTN_UP,   false, 1000, "UP   单击 -> 菜单光标 2->1 (I2C)" },
    { BTN_UP,   true,  1000, "UP   长按 -> 进入下一级（选中 I2C -> page 7）" },
    { BTN_DOWN, false, 1000, "DOWN 单击 -> 同级前移 page 6 (SPI)" },
    { BTN_UP,   false, 1000, "UP   单击 -> 同级后移 page 7 (I2C)" },
    { BTN_DOWN, true,  1000, "DOWN 长按 -> 返回上一级（回菜单，光标留在 I2C）" },
    { BTN_UP,   true,  1000, "UP   长按 -> 再次进入（I2C）" },
    { BTN_BACK, false, 1000, "BACK 单击 -> 返回主菜单" },
    { BTN_BACK, true,  1000, "BACK 长按 -> 恢复 AUTO 轮播" },
};

static void selftest_task(void *arg)
{
    (void)arg;
    const int n = (int)(sizeof(s_steps) / sizeof(s_steps[0]));
    ESP_LOGW(TAG, "=== 自检开始：注入 %d 个合成按键事件（不碰物理引脚）===", n);
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (int i = 0; i < n; i++) {
        vTaskDelay(pdMS_TO_TICKS(s_steps[i].delay_ms));
        ESP_LOGW(TAG, "[注入 %d/%d] %s", i + 1, n, s_steps[i].desc);
        push(s_steps[i].id, s_steps[i].lg);
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "=== 自检结束：全程无异常则按键链路完好 ===");
    vTaskDelete(NULL);
}
#endif

esp_err_t btn_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_MAX; i++) mask |= (1ULL << s_pin[i]);

    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 内部上拉兜底 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      /* 轮询，不用中断 */
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config 失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 初值对齐，避免上电瞬间误报按键 */
    for (int i = 0; i < BTN_MAX; i++) {
        uint8_t lv = (gpio_get_level(s_pin[i]) == BTN_ACTIVE_LEVEL) ? 1 : 0;
        s_dec[i].last_raw = lv;
        s_dec[i].stable   = lv;         /* cnt/其余字段 = 0：等真实的连续采样 */
        ESP_LOGI(TAG, "%s -> GPIO%d  初始电平=%d", s_name[i], s_pin[i], lv);
    }

    xTaskCreate(btn_task, "btn", 3072, NULL, 5, NULL);
#if BTN_SELFTEST
    xTaskCreate(selftest_task, "btntest", 3072, NULL, 4, NULL);
#endif
    ESP_LOGI(TAG, "3 键就绪：GPIO%d/%d/%d | 去抖 按下%dms/抬起%dms | 长按 %dms | 真松开 %dms",
             BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_BACK,
             BTN_PRESS_N * BTN_POLL_MS, BTN_RELEASE_N * BTN_POLL_MS,
             BTN_LONG_PRESS_MS, BTN_TRUE_RELEASE_N * BTN_POLL_MS);
    return ESP_OK;
}
