/**
 * ui_cfg.c — UI 配置持久化层（NVS）实现
 *
 * 见 ui_cfg.h 的设计说明。判据核心 = **写入 → 回读 → 逐位比对**。
 *
 * ⚠️ 结构上刻意分成三层，别合并：
 *     nvs_read_raw()  纯读，无副作用（不碰时间基准、不碰运行期缓存）
 *     ui_cfg_init()   读 + 校验 + 立时间基准 + 更新缓存   ← 有副作用
 *     ui_cfg_save()   写 + 用 nvs_read_raw 回读对拍
 * 踩过的坑：回读若走 ui_cfg_init()，会把 s_base_us 重置成「此刻」，
 * 时钟每被读一次就冻结一次。回读必须走无副作用那条路。
 */

#include "ui_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ui_cfg";

/* ---- 键名 ---- */
static const char K_VER[]   = "cfg_ver";
static const char K_LANG[]  = "lang";
static const char K_EPOCH[] = "epoch";
static const char K_SNTP[]  = "sntp";

/* ---- 运行期状态 ---- */
static int64_t  s_base_epoch = 0;      /**< 时间基准（Unix 秒）；<=0 = 未设定 */
static int64_t  s_base_us    = 0;      /**< 基准建立时的 esp_timer 读数 */
static bool     s_nvs_ready  = false;
static ui_cfg_t s_cfg;                 /**< 运行期副本 —— getter 不每次碰 flash */
static bool     s_cfg_valid  = false;

static void defaults(ui_cfg_t *c)
{
    c->ver   = UI_CFG_VER;
    c->lang  = 0;   /* 出厂默认中文 */
    c->sntp  = 0;   /* 出厂默认关（纯手动） */
    c->_pad  = 0;
    c->epoch = 0;   /* 未设定 —— P1 首启向导走完才有 */
}

/* ⚠️⚠️ 绝不用 memcmp 比结构体 ⚠️⚠️
 * ui_cfg_t 里 int64_t 要求 8 字节对齐，于是 _pad(偏移3) 与 epoch(偏移8) 之间
 * 存在 4 字节**对齐空洞**。结构体初始化不会写它，两边的空洞各是各自的栈垃圾，
 * memcmp 必然失败 —— 而逐个字段打印出来却「全都一样」。
 * 这个坑真踩过：selftest 报 "对拍失败：ver 1/1 lang 1/1 sntp 1/1 epoch 相等"。
 * 所以只比数据字段，填充字节不是数据。 */
static bool cfg_eq(const ui_cfg_t *a, const ui_cfg_t *b)
{
    return a->ver == b->ver && a->lang == b->lang &&
           a->sntp == b->sntp && a->epoch == b->epoch;
}

/* ═══════════════════════ NVS 基础设施 ═══════════════════════ */

/** 幂等地保证 NVS 可用。false = 存储层坏了（上层走「不可持久化」降级）。 */
static bool nvs_ensure(void)
{
    if (s_nvs_ready) return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 分区坏了：擦掉重来（esp-idf 官方范式，不是掩盖问题） */
        ESP_LOGW(TAG, "nvs 分区异常(%s)，擦除重建", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return false;
    }
    s_nvs_ready = true;
    return true;
}

/* ═══════════════════════ 纯读（无副作用） ═══════════════════════ */

/**
 * 直接从 NVS 读一份配置出来，**不碰时间基准、不碰运行期缓存**。
 * @return true = 读到合法配置；false = 无记录/残缺/版本不符/越界已回落
 */
static bool nvs_read_raw(ui_cfg_t *out)
{
    ui_cfg_t c;
    defaults(&c);
    if (!nvs_ensure()) { if (out) *out = c; return false; }

    nvs_handle_t h;
    if (nvs_open(UI_CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        if (out) *out = c;
        return false;
    }

    uint8_t v8 = 0;
    int64_t e64 = 0;
    esp_err_t rv = nvs_get_u8 (h, K_VER,   &v8);
    esp_err_t rl = nvs_get_u8 (h, K_LANG,  &c.lang);
    esp_err_t re = nvs_get_i64(h, K_EPOCH, &e64);
    esp_err_t rs = nvs_get_u8 (h, K_SNTP,  &c.sntp);
    nvs_close(h);

    if (rv != ESP_OK || rl != ESP_OK || re != ESP_OK || rs != ESP_OK) {
        ESP_LOGW(TAG, "配置残缺(ver=%s lang=%s epoch=%s sntp=%s)",
                 esp_err_to_name(rv), esp_err_to_name(rl),
                 esp_err_to_name(re), esp_err_to_name(rs));
        if (out) *out = c;
        return false;
    }
    if (v8 != UI_CFG_VER) {
        ESP_LOGW(TAG, "版本不符(存=%u 期望=%u) ⇒ 当首启重走向导", v8, UI_CFG_VER);
        if (out) *out = c;
        return false;
    }
    if (c.lang > 1) {                       /* 越界语言 ⇒ 回落中文，不崩 */
        ESP_LOGW(TAG, "lang 越界(%u) ⇒ 回落中文", c.lang);
        c.lang = 0;
    }
    if (e64 < UI_CFG_MIN_EPOCH) e64 = 0;    /* 时间不合理 ⇒ 当未设定 */

    c.ver   = UI_CFG_VER;
    c._pad  = 0;
    c.epoch = e64;
    if (out) *out = c;
    return true;
}

/* ═══════════════════════ 载入 ═══════════════════════ */

bool ui_cfg_init(ui_cfg_t *out)
{
    ui_cfg_t c;
    bool ok = nvs_read_raw(&c);

    s_cfg        = c;             /* 缓存总是更新：即使判首启，缓存也应是出厂默认 */
    s_cfg_valid  = true;
    s_base_epoch = c.epoch;       /* 立时间基准 */
    s_base_us    = esp_timer_get_time();
    if (out) *out = c;

    if (ok) ESP_LOGI(TAG, "载入配置 ver=%u lang=%u sntp=%u epoch=%lld",
                     c.ver, c.lang, c.sntp, (long long)c.epoch);
    else    ESP_LOGI(TAG, "首启（出厂默认：中文 / SNTP关 / 时间未定）");
    return ok;
}

/* ═══════════════════════ 写（含写入回读对拍） ═══════════════════════ */

esp_err_t ui_cfg_save(const ui_cfg_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (!nvs_ensure()) return ESP_FAIL;
    if (in->lang > 1) return ESP_ERR_INVALID_ARG;   /* 挡掉越界值落盘 */

    ui_cfg_t c = *in;
    c.ver   = UI_CFG_VER;
    c._pad  = 0;
    if (c.epoch < 0) c.epoch = 0;                  /* -1（未定）落盘成 0 */

    nvs_handle_t h;
    esp_err_t err = nvs_open(UI_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err  = nvs_set_u8 (h, K_VER,   c.ver);
    err |= nvs_set_u8 (h, K_LANG,  c.lang);
    err |= nvs_set_i64(h, K_EPOCH, c.epoch);
    err |= nvs_set_u8 (h, K_SNTP,  c.sntp);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 写入回读逐位对拍 ──
     * 走 nvs_read_raw() 而非 ui_cfg_init()：对拍只该证明「落盘了」，
     * 不该顺带重置时间基准。 */
    ui_cfg_t back;
    if (!nvs_read_raw(&back)) {
        ESP_LOGE(TAG, "对拍失败：刚写完却读不回来");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg_eq(&back, &c)) {
        ESP_LOGE(TAG, "对拍失败：ver %u/%u lang %u/%u sntp %u/%u epoch %lld/%lld",
                 back.ver, c.ver, back.lang, c.lang, back.sntp, c.sntp,
                 (long long)back.epoch, (long long)c.epoch);
        return ESP_ERR_INVALID_STATE;
    }

    /* 落盘确认 + 以新 epoch 为基准，让时间继续往前走 */
    s_cfg        = c;
    s_cfg_valid  = true;
    s_base_epoch = c.epoch;
    s_base_us    = esp_timer_get_time();
    ESP_LOGI(TAG, "保存成功并回读一致 ver=%u lang=%u sntp=%u epoch=%lld",
             c.ver, c.lang, c.sntp, (long long)c.epoch);
    return ESP_OK;
}

void ui_cfg_factory_reset(void)
{
    if (nvs_ensure()) {
        nvs_handle_t h;
        if (nvs_open(UI_CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t e = nvs_erase_all(h);
            nvs_close(h);
            if (e != ESP_OK) ESP_LOGE(TAG, "恢复出厂擦除失败: %s", esp_err_to_name(e));
        }
    }
    ESP_LOGW(TAG, "恢复出厂完成，重启");
    fflush(stdout);
    esp_restart();
}

/* ═══════════════════════ 时间基准 ═══════════════════════ */

int64_t ui_cfg_now(void)
{
    if (s_base_epoch <= 0) return -1;   /* 时间未定（首启向导走完才有） */
    return s_base_epoch + (esp_timer_get_time() - s_base_us) / 1000000;
}

void ui_cfg_set_now(int64_t e)
{
    s_base_epoch = (e < UI_CFG_MIN_EPOCH) ? 0 : e;
    s_base_us    = esp_timer_get_time();
    s_cfg.epoch  = s_base_epoch;
}

/* ═══════════════════════ 便捷存取 ═══════════════════════ */

/** 取当前配置（epoch 字段为「此刻」或 0=未定）。绝不碰 flash。 */
static ui_cfg_t cur(void)
{
    if (!s_cfg_valid) {
        ui_cfg_t c;
        defaults(&c);
        s_cfg = c;
        s_cfg_valid = true;
    }
    int64_t now = ui_cfg_now();
    s_cfg.epoch = (now > 0) ? now : 0;
    return s_cfg;
}

uint8_t ui_cfg_lang_get(void) { return cur().lang; }
uint8_t ui_cfg_sntp_get(void) { return cur().sntp; }

esp_err_t ui_cfg_set_lang(uint8_t lang)
{
    if (lang > 1) return ESP_ERR_INVALID_ARG;
    ui_cfg_t c = cur();
    c.lang = lang;
    return ui_cfg_save(&c);
}

esp_err_t ui_cfg_set_sntp(uint8_t on)
{
    ui_cfg_t c = cur();
    c.sntp = on ? 1 : 0;
    return ui_cfg_save(&c);
}

/* ═══════════════════════ 写入回读逐位对拍自检 ═══════════════════════ */

int ui_cfg_selftest(void)
{
    if (!nvs_ensure()) { ESP_LOGE(TAG, "[selftest] NVS 不可用"); return 1; }

    int fails = 0;

    /* ---- 0. 备份原状（自检绝不能毁掉用户配置） ---- */
    ui_cfg_t orig;
    bool     had = ui_cfg_init(&orig);

    /* ---- 1. 写入 → 回读 → 逐位对拍（正向） ---- */
    ui_cfg_t a = { UI_CFG_VER, 1, 1, 0, 1780000000LL };
    if (ui_cfg_save(&a) != ESP_OK) { ESP_LOGE(TAG, "[selftest 1] 写入失败"); fails++; }
    ui_cfg_t ra;
    if (!ui_cfg_init(&ra)) { ESP_LOGE(TAG, "[selftest 1] 回读不到"); fails++; }
    else if (!cfg_eq(&ra, &a)) {
        ESP_LOGE(TAG, "[selftest 1] 回读不一致 ver %u/%u lang %u/%u sntp %u/%u epoch %lld/%lld",
                 ra.ver, a.ver, ra.lang, a.lang, ra.sntp, a.sntp,
                 (long long)ra.epoch, (long long)a.epoch);
        fails++;
    } else ESP_LOGI(TAG, "[selftest 1] 写入回读逐位一致 PASS");

    /* ---- 2. 二次写入不同值：抓「改了但读到旧值」（陈旧读） ---- */
    ui_cfg_t b = { UI_CFG_VER, 0, 0, 0, 1700000000LL };
    if (ui_cfg_save(&b) != ESP_OK) { ESP_LOGE(TAG, "[selftest 2] 写入失败"); fails++; }
    ui_cfg_t rb;
    if (!ui_cfg_init(&rb) || !cfg_eq(&rb, &b)) {
        ESP_LOGE(TAG, "[selftest 2] 二次写入没生效（读到陈旧值）"); fails++;
    } else ESP_LOGI(TAG, "[selftest 2] 二次写入生效 PASS");

    /* ---- 3. 擦除后必须真的读不到：抓「首启判定恒假」 ---- */
    nvs_handle_t h;
    if (nvs_open(UI_CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_close(h);
    }
    ui_cfg_t re;
    if (ui_cfg_init(&re)) { ESP_LOGE(TAG, "[selftest 3] 擦除后仍读到配置"); fails++; }
    else ESP_LOGI(TAG, "[selftest 3] 擦除后确实读不到 ⇒ 首启判定 PASS");

    /* ---- 4. 还原原状 ---- */
    if (had) {
        if (ui_cfg_save(&orig) != ESP_OK) {
            ESP_LOGE(TAG, "[selftest 4] 还原原配置失败！"); fails++;
        }
    }
    ui_cfg_t chk;
    bool ok = ui_cfg_init(&chk);
    if (ok != had || (had && !cfg_eq(&chk, &orig))) {
        ESP_LOGE(TAG, "[selftest 4] 原状未还原"); fails++;
    } else ESP_LOGI(TAG, "[selftest 4] 原状已还原 PASS");

    ESP_LOGI(TAG, "ui_cfg selftest: fails=%d", fails);
    return fails;
}

/* ═══════════════ 跨重启持久化探针（判据「断电重启设置不丢」的真形态） ═══════════════
 * 上一段 selftest 只证明「同一次开机内 写→回读 一致」—— 那证不了掉电。
 * 本探针把「写入」和「验证」拆到**两次开机**：
 *     第 1 次开机：写一份可辨认的配置 + 打标记，然后停下
 *     第 2 次开机：看到标记 ⇒ 必须把那份配置原样读回来
 * 中间隔一次硬复位（flash 重新上电读取）⇒ 这才真的过了 flash 介质这一层。
 */
#if UI_CFG_PERSIST_TEST
#define PST_MAGIC  0x50455355u      /* "PESU" —— 同时当「标记值」校验，只查存在不查值
                                     * 会让任意同名脏键走进校验分支（假判据）。
                                     * 改动此常量即可让旧标记失效、强制重跑写入轮。 */
#define PST_EPOCH  1780000000LL
static const char K_PST[] = "pst";

int ui_cfg_persist_probe(void)
{
    ui_cfg_t c;
    bool have_cfg = ui_cfg_init(&c);

    uint32_t magic = 0;
    nvs_handle_t h;
    bool have_magic = false;
    if (nvs_open(UI_CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        have_magic = (nvs_get_u32(h, K_PST, &magic) == ESP_OK) && (magic == PST_MAGIC);
        nvs_close(h);
    }

    /* ---- 第 2 次开机：验证上一轮写的还在不在 ---- */
    if (have_magic) {
        if (!have_cfg) { ESP_LOGE(TAG, "[persist] FAIL 有标记但配置读不到"); return 1; }
        if (c.lang != 1 || c.sntp != 1 || c.epoch != PST_EPOCH) {
            ESP_LOGE(TAG, "[persist] FAIL 配置被改/丢失 lang=%u sntp=%u epoch=%lld"
                           " (期望 lang=1 sntp=1 epoch=%lld)",
                     c.lang, c.sntp, (long long)c.epoch, (long long)PST_EPOCH);
            return 1;
        }
        ESP_LOGW(TAG, "[persist] PASS 配置跨重启原样存活 lang=1 sntp=1 epoch=%lld",
                 (long long)c.epoch);
        return 0;
    }

    /* ---- 第 1 次开机：写入 + 打标记 ---- */
    ui_cfg_t w = { UI_CFG_VER, 1, 1, 0, PST_EPOCH };
    if (ui_cfg_save(&w) != ESP_OK) {
        ESP_LOGE(TAG, "[persist] 写入失败"); return 1;
    }
    if (nvs_open(UI_CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = nvs_set_u32(h, K_PST, PST_MAGIC);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
        if (e != ESP_OK) { ESP_LOGE(TAG, "[persist] 标记写入失败: %s", esp_err_to_name(e)); return 1; }
    } else { ESP_LOGE(TAG, "[persist] 打开 NVS 失败"); return 1; }

    ESP_LOGW(TAG, "[persist] 第1次开机：已写入 lang=1(English) sntp=1 epoch=%lld"
                  " ⇒ 请硬复位后复验", (long long)PST_EPOCH);
    return 0;
}
#endif /* UI_CFG_PERSIST_TEST */
