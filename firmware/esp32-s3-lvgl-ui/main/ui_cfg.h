/**
 * ui_cfg.h / ui_cfg.c — UI 配置持久化层（NVS）
 *
 * ═══ P0 判据之二（docs/UI_V2_PLAN.md:195「断电重启设置不丢」）的实现 ═══
 * 规划原文（:155-161）：
 *     NVS namespace: "ui_cfg"
 *       cfg_ver   u8    版本号，不存在 ⇒ 首启 ⇒ 走向导
 *       lang      u8    0=中文 1=English
 *       epoch     i64   设定时刻（掉电不丢，无 RTC 时靠它恢复）
 *       sntp      u8    0/1
 *       -- 恢复出厂 = nvs_erase_all("ui_cfg") + esp_restart() --
 *
 * ⚠️ **无 RTC 的诚实说明**（规划 :163 已写明，这里照做不做美化）：
 *     存的是「设定那一刻的绝对时刻」，运行期用 esp_timer 推进。
 *     掉电期间不走时 —— 无 RTC 做不到，不是 bug 是物理事实。
 *     SNTP 开启且联网时可自动纠正。
 *
 * ⚠️ **本模块不依赖 i18n**：存储与呈现分离，语言切换由 app 层把
 *     cfg.lang 灌进 g_ui_lang。这样自检不必拉起 i18n。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 配置结构体。字段顺序 = 落盘顺序，改动必须同步 UI_CFG_VER。 */
typedef struct {
    uint8_t ver;      /**< 版本号；与 UI_CFG_VER 不符 ⇒ 当作首启（升级即重走向导） */
    uint8_t lang;     /**< 0=中文 1=English（i18n.h 的 UI_LANG_ZH/EN） */
    uint8_t sntp;     /**< 0=关（纯手动） 1=开 */
    uint8_t _pad;     /**< 显式补白：让 memcmp 对拍不依赖结构体尾部填充 */
    int64_t epoch;    /**< 设定那一刻的绝对时刻（Unix 秒） */
} ui_cfg_t;

/** 当前存储格式版本。升级时改这里，旧版本记录会被判为首启。 */
#define UI_CFG_VER       1u
#define UI_CFG_NS        "ui_cfg"

/** epoch 合理性下界：2020-01-01T00:00:00Z。早于它的一律视为「没设过」。 */
#define UI_CFG_MIN_EPOCH 1577836800LL

/**
 * 初始化 NVS 并载入配置。
 * @return true  = 读到了合法配置（out 已填）
 *         false = 首启 / 版本不符 / 记录损坏（out 已填出厂默认中文）
 * 可重复调用（幂等）。
 */
bool ui_cfg_init(ui_cfg_t *out);

/** 整份写入并 commit。内部会做「写入→回读→逐位比对」，不一致直接返回错误。 */
esp_err_t ui_cfg_save(const ui_cfg_t *in);

/** 恢复出厂：擦掉整个 ui_cfg 命名空间后立即重启。 */
void ui_cfg_factory_reset(void);

/* ---- 运行期时间基准（epoch + esp_timer 推进） ---- */
int64_t ui_cfg_now(void);            /**< 当前推定时刻（Unix 秒） */
void    ui_cfg_set_now(int64_t e);   /**< 改时间基准（用户设时间 / SNTP 纠正后调用） */

/* ---- 便捷存取 ---- */
uint8_t     ui_cfg_lang_get(void);
esp_err_t   ui_cfg_set_lang(uint8_t lang);
uint8_t     ui_cfg_sntp_get(void);
esp_err_t   ui_cfg_set_sntp(uint8_t on);

/**
 * 写入回读逐位对拍自检。
 * 会先备份现有配置，做完恢复原状（原状是「首启」就恢复到首启态）。
 * @return 失败项数，0 = 全部通过
 */
int ui_cfg_selftest(void);

#if UI_CFG_PERSIST_TEST
/**
 * 跨重启持久化探针（仅排查用）。必须**跨两次开机**才有意义：
 *   第 1 次开机调用 → 写入配置并打标记
 *   硬复位后第 2 次开机调用 → 校验上一轮写的原样还在
 * @return 0 = PASS / 该次是写入
 */
int ui_cfg_persist_probe(void);
#endif

#ifdef __cplusplus
}
#endif
