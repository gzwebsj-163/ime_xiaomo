/**
 * wifi_sta.c — 真实 WiFi STA 连接层（实现）
 *
 * 生命周期（全部在 wifi_mgr_task 里串行执行，互不并发）：
 *
 *   app_main ──wifi_st_start()──▶ (立即返回)
 *                                   │ 建 netif / event loop / 驱动
 *                                   ▼
 *                          ┌─ 定向扫描（只扫目标 SSID）
 *                          │   → 命中 = 该 AP 确实在 2.4GHz 上
 *                          │   → 空   = 它是 5GHz / 太远 / 隐藏（关键判据！）
 *                          ├─ 全量扫描 → 打日志列出附近 AP（方便换 2.4G 的 SSID）
 *                          └─ esp_wifi_connect() → 等 GOT_IP
 *                                   │
 *                    ┌──────────────┴──────────────┐
 *              GOT_IP（成功）               DISCONNECTED（失败/掉线）
 *                    │                             │
 *              存 IP/RSSI/信道             记 reason + retries++，3s 后重连
 *
 * ⚠️ 两处「必须这么写」的理由：
 *   1. 扫描用 **阻塞式**（scan_start(..., true)）但**绝不放在事件回调里** ——
 *      事件回调跑在 sys_evt 任务上，阻塞 2~3s 会顶住整个 WiFi 事件循环。
 *      所以扫描/连接统一放在自己的 wifi_mgr_task 里。
 *   2. 状态用**互斥量 + 拷贝**暴露给 UI —— LVGL 任务与 WiFi 事件任务是两个线程，
 *      共享字符串必须上锁，绝不能把结构体指针直接递给 UI。
 */
#include "wifi_sta.h"
#include "wifi_cfg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "wifi"

#define NVS_NS    "wifi_cfg"
#define KEY_SSID  "ssid"
#define KEY_PASS  "pass"

#define SCAN_MAX  64          /* 全量扫描最多保留多少条（够看就行） */
#define RETRY_MS  3000        /* 断线重连间隔 */


/* ---------------- 共享状态（受 s_mux 保护） ---------------- */
static wifi_st_status_t  s_st;
static SemaphoreHandle_t s_mux;
static esp_netif_t      *s_netif;
static char              s_ssid[33];
static char              s_pass[65];

static void st_lock(void)   { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
static void st_unlock(void) { if (s_mux) xSemaphoreGive(s_mux); }

void wifi_st_get(wifi_st_status_t *out)
{
    if (!out) return;
    st_lock();
    *out = s_st;
    st_unlock();
}

/* ---------------- reason 码翻译 ---------------- */
const char *wifi_st_reason_str(int reason)
{
    switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "NOT_AUTHED";
    case 6:   return "NOT_ASSOCED";
    case 7:   return "ASSOC_LEAVE";
    case 8:   return "ASSOC_NOT_AUTHED";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT(PASS?)";
    case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
    case 23:  return "802_1X_AUTH_FAIL";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL(PASS?)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "?";
    }
}

/* ---------------- NVS：凭据读写 ---------------- */
static void cred_load(void)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", WIFI_CFG_SSID);   /* 默认值打底 */
    snprintf(s_pass, sizeof(s_pass), "%s", WIFI_CFG_PASS);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS 无凭据 → 用默认 SSID [%s]", s_ssid);
        return;
    }
    char buf[65];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, KEY_SSID, buf, &len) == ESP_OK && buf[0]) {
        /* ⚠️ 必须限宽：SSID 最长 32 字节，而 NVS 读出来的 buf 有 65 ——
         * 直接用 %s 会被 -Werror=format-truncation 拦下（也可能真截断） */
        snprintf(s_ssid, sizeof(s_ssid), "%.32s", buf);
        len = sizeof(buf);
        if (nvs_get_str(h, KEY_PASS, buf, &len) == ESP_OK) {
            snprintf(s_pass, sizeof(s_pass), "%.64s", buf);
        }
        ESP_LOGI(TAG, "NVS 命中凭据 → SSID [%s]", s_ssid);
    } else {
        ESP_LOGI(TAG, "NVS 无凭据 → 用默认 SSID [%s]", s_ssid);
    }
    nvs_close(h);
}

static void cred_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, KEY_SSID, s_ssid);
    nvs_set_str(h, KEY_PASS, s_pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 事件回调（运行在 sys_evt 任务，必须短） ---------------- */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id == WIFI_EVENT_STA_START) {
        /* 真正的工作交给 wifi_mgr_task（扫描+连接），此处只记状态 */
        ESP_LOGI(TAG, "STA 驱动就绪，交由 mgr 任务扫描+连接");
        st_lock(); s_st.state = WIFI_ST_SCANNING; st_unlock();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        int r = e ? e->reason : -1;

        st_lock();
        s_st.last_reason = r;
        s_st.retries++;
        s_st.ip[0] = '\0';
        s_st.state = WIFI_ST_FAIL;
        uint32_t n = s_st.retries;
        st_unlock();

        ESP_LOGW(TAG, "✗ 连接断开 reason=%d (%s) 累计第 %u 次 → %d ms 后重连",
                 r, wifi_st_reason_str(r), (unsigned)n, RETRY_MS);
        if (r == 201) {
            ESP_LOGW(TAG, "   ↳ NO_AP_FOUND：2.4GHz 上扫不到该 SSID。"
                          "若 AP 只在 5GHz，ESP32-S3 永远连不上（无 5GHz 射频）");
        } else if (r == 15 || r == 202) {
            ESP_LOGW(TAG, "   ↳ 认证/四次握手失败：**大概率密码不对**（或加密方式不兼容）");
        }
        return;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));

    /* 顺手取一下 AP 信息：信道 + 信号强度 */
    wifi_ap_record_t ap = {0};
    int rssi = 0, ch = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { rssi = ap.rssi; ch = ap.primary; }

    st_lock();
    snprintf(s_st.ip, sizeof(s_st.ip), "%s", ip);
    s_st.rssi = rssi;
    s_st.channel = ch;
    s_st.state = WIFI_ST_GOT_IP;
    s_st.retries = 0;
    s_st.last_reason = 0;
    st_unlock();

    ESP_LOGI(TAG, "✔ 已连接 SSID [%s]  IP %s  RSSI %d dBm  CH %d", s_ssid, ip, rssi, ch);
    cred_save();   /* 连上即持久化，下次开机不用再配置 */
}

/* ---------------- 两个扫描 ---------------- */
static void scan_targeted(void)
{
    /* ① 定向扫描：直接问「这个 SSID 在不在 2.4GHz 上」——
     *    这比全量扫描后再过滤更硬，空结果就是强证据。 */
    wifi_scan_config_t sc = {0};
    sc.ssid = (uint8_t *)s_ssid;
    sc.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "定向扫描启动失败: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGE(TAG, "✗ 定向扫描：[%s] 在 2.4GHz 上**扫不到**！", s_ssid);
        ESP_LOGE(TAG, "  可能原因：①该 AP 是纯 5GHz（S3 无 5GHz 射频，物理上连不上）"
                      " ②距离太远 ③SSID 隐藏 ④名字拼写不同");
        st_lock(); s_st.ap_seen = false; st_unlock();
        esp_wifi_clear_ap_list();
        return;
    }

    wifi_ap_record_t *rec = calloc(n, sizeof(wifi_ap_record_t));
    if (rec) {
        uint16_t m = n;
        if (esp_wifi_scan_get_ap_records(&m, rec) == ESP_OK) {
            for (int i = 0; i < m; i++) {
                ESP_LOGI(TAG, "✓ 定向扫描命中 [%s] RSSI %d dBm CH %d auth %d",
                         (const char *)rec[i].ssid, rec[i].rssi,
                         rec[i].primary, (int)rec[i].authmode);
            }
        }
        free(rec);
    }
    st_lock(); s_st.ap_seen = true; st_unlock();
    esp_wifi_clear_ap_list();
}

static void scan_dump(void)
{
    /* ② 全量扫描：把附近能看到的 2.4GHz AP 打出来。
     *    用途 = 如果目标连不上，日志里直接能挑到同名的 2.4GHz SSID。 */
    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    st_lock(); s_st.scan_n = (int)n; st_unlock();

    if (n == 0) { ESP_LOGW(TAG, "全量扫描 0 个 AP（射频/天线异常？）"); esp_wifi_clear_ap_list(); return; }
    uint16_t cap = (n > SCAN_MAX) ? SCAN_MAX : n;
    wifi_ap_record_t *rec = calloc(cap, sizeof(wifi_ap_record_t));
    if (rec) {
        uint16_t m = cap;
        if (esp_wifi_scan_get_ap_records(&m, rec) == ESP_OK) {
            for (int i = 0; i < m; i++) {                 /* 按 RSSI 降序（插入排序） */
                for (int j = i + 1; j < m; j++) {
                    if (rec[j].rssi > rec[i].rssi) {
                        wifi_ap_record_t t = rec[i]; rec[i] = rec[j]; rec[j] = t;
                    }
                }
            }
            ESP_LOGI(TAG, "全量扫描：2.4GHz 可见 %u 个 AP（列出前 %u）", (unsigned)n, (unsigned)m);
            for (int i = 0; i < m; i++) {
                ESP_LOGI(TAG, "  [%2d] %-28s %4d dBm  CH%2d  auth%d",
                         i + 1, (const char *)rec[i].ssid,
                         rec[i].rssi, rec[i].primary, (int)rec[i].authmode);
            }
        }
        free(rec);
    }
    esp_wifi_clear_ap_list();
}

/* ---------------- 管理任务：初始化 → 扫描 → 连接（串行，无并发） ---------------- */
/* ---------------- 密码候选探测（仅排查用，WIFI_PASS_PROBE=1 打开） ----------------
 *
 * 起因：用户给的密码 `34023077aa..` 末尾带两个点，**疑似是「省略号」而非密码内容**。
 * 真机症状 = reason 15 (4WAY_HANDSHAKE_TIMEOUT) 在 WPA2 下几乎就是「密码不对」的铁证，
 * 且扫描显示该 AP 是 auth4 = WPA/WPA2-PSK 混合（**不含 WPA3**），
 * 所以可以排除「WPA3-SAE 协商失败」这条岔路 —— 只剩密码一个变量。
 *
 * 策略：把候选密码逐个灌进驱动，每个等 CAND_WAIT_MS，谁先拿到 IP 就是它。
 * 同时把密码按字节打 hex，防止「肉眼看不出」的不可见字符。 */
#if WIFI_PASS_PROBE
static const char *s_cand[] = {
    "34023077aa..",   /* ① 原样（字面 13 位） */
    "34023077aa",     /* ② 去掉两个点（尾部两点是省略号） */
    "34023077aa.",    /* ③ 只去掉一个点 */
    "34023077AA..",   /* ④ 原样但大写 AA（部分路由器密码区分大小写，防手抄错） */
};
#define CAND_N        (sizeof(s_cand) / sizeof(s_cand[0]))
#define CAND_WAIT_MS  15000     /* 单个候选最长等待 */

static void cand_dump_hex(const char *s)
{
    char hex[3 * 66] = {0};
    int n = (int)strlen(s), o = 0;
    for (int i = 0; i < n && o < (int)sizeof(hex) - 4; i++)
        o += snprintf(hex + o, sizeof(hex) - o, "%02X ", (unsigned char)s[i]);
    if (n == 0) snprintf(hex, sizeof(hex), "(空)");
    ESP_LOGW(TAG, "      字节: %s [长度 %d]", hex, n);
}

/* 等待「拿到 IP」；返回 true=成功 */
static bool cand_wait_got_ip(void)
{
    for (int i = 0; i < CAND_WAIT_MS / 250; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        st_lock();
        wifi_st_state_t st = s_st.state;
        st_unlock();
        if (st == WIFI_ST_GOT_IP) return true;
    }
    return false;
}

static void password_probe(const wifi_config_t *base)
{
    ESP_LOGW(TAG, "════════ 密码候选探测开始（%d 个候选，各等 %d ms）════════",
             (int)CAND_N, CAND_WAIT_MS);

    for (int i = 0; i < (int)CAND_N; i++) {
        wifi_config_t wc = *base;
        memset(wc.sta.password, 0, sizeof(wc.sta.password));
        snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", s_cand[i]);

        ESP_LOGW(TAG, "── 候选 %d/%d: \"%s\" ──", i + 1, (int)CAND_N, s_cand[i]);
        cand_dump_hex(s_cand[i]);

        st_lock();
        s_st.retries = 0;
        s_st.last_reason = 0;
        s_st.ip[0] = '\0';
        s_st.state = WIFI_ST_CONNECTING;
        st_unlock();

        esp_wifi_disconnect();                    /* 可能本来就没连上，忽略返回码 */
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (e != ESP_OK) { ESP_LOGE(TAG, "set_config 失败: %s", esp_err_to_name(e)); continue; }
        esp_wifi_connect();

        if (cand_wait_got_ip()) {
            st_lock();
            char ip[16];
            snprintf(ip, sizeof(ip), "%s", s_st.ip);
            st_unlock();
            ESP_LOGI(TAG, "★★★★★★★★ 候选 %d 命中！密码 = \"%s\"  IP = %s",
                     i + 1, s_cand[i], ip);
            snprintf(s_pass, sizeof(s_pass), "%s", s_cand[i]);
            cred_save();                          /* 成功的密码落 NVS */
            ESP_LOGW(TAG, "════════ 探测结束：用这一个，其余候选跳过 ════════");
            return;
        }

        st_lock();
        int r = s_st.last_reason;
        uint32_t n = s_st.retries;
        st_unlock();
        ESP_LOGW(TAG, "✗ 候选 %d 失败：last_reason=%d (%s)，断线 %u 次",
                 i + 1, r, wifi_st_reason_str(r), (unsigned)n);
    }

    ESP_LOGE(TAG, "════════ 全部 %d 个候选均失败 ════════", (int)CAND_N);
    ESP_LOGE(TAG, "  说明密码不在候选里（或 AP 有 MAC 白名单/需要按按钮配对）。");
    ESP_LOGE(TAG, "  请提供完整密码，或让我加一个「扫描 MAC + 旁路」的诊断。");
}
#endif  /* WIFI_PASS_PROBE */

static void wifi_mgr_task(void *arg)
{
    (void)arg;

    /* 1) NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err)); }

    cred_load();
    st_lock(); snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", s_ssid); st_unlock();

    /* 2) netif + 事件循环 + STA 驱动 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { ESP_LOGE(TAG, "create_default_wifi_sta 失败"); vTaskDelete(NULL); return; }

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* 凭据我们自己存 NVS */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wc = {0};
    /* 限宽：wifi_config 里 ssid 是 32 字节、password 是 64 字节（含结尾 NUL），
     * 我们的缓冲区各多 1 字节，直接 %s 会被 -Werror=format-truncation 拦下 */
    snprintf((char *)wc.sta.ssid,     sizeof(wc.sta.ssid),     "%.31s", s_ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%.63s", s_pass);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  /* AP 实测 auth4=WPA/WPA2-PSK 混合（不含 WPA3），
                                                      * 明确限到 WPA2 可排除 WPA3-SAE 协商这条岔路 */
    wc.sta.pmf_cfg.capable = false;                   /* 不启用 PMF：非 802.11w 强制场景下更兼容 */
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);   /* 调试器场景要低延迟，关省电 */

    st_lock(); s_st.state = WIFI_ST_STARTING; st_unlock();

    /* 3) 启动诊断：先证明「这个 SSID 到底在不在 2.4GHz 上」 */
    ESP_LOGI(TAG, "==== 启动扫描诊断 ====");
    st_lock(); s_st.state = WIFI_ST_SCANNING; st_unlock();
    scan_targeted();
    scan_dump();
    st_lock(); s_st.scan_done = true; st_unlock();   /* 扫描结论已定，UI 可以据此判断了 */
    ESP_LOGI(TAG, "==== 扫描结束，开始连接 ====");

    /* 4) 连接 + 无限重连（事件里只记状态，真正的重连由本任务驱动，
     *    这样即使事件回调被挤爆也不会丢重试逻辑） */
    st_lock(); s_st.state = WIFI_ST_CONNECTING; st_unlock();

#if WIFI_PASS_PROBE
    /* 排查模式：轮到每个候选密码去试，成功后落 NVS，然后进下面的保活循环 */
    password_probe(&wc);
    st_lock(); s_st.retries = 0; st_unlock();
    esp_wifi_connect();          /* 保底：若探测全失败也用最后一个候选继续重连 */
#else
    esp_wifi_connect();
#endif

    uint32_t last_retries = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        st_lock();
        wifi_st_state_t st = s_st.state;
        uint32_t r = s_st.retries;
        st_unlock();

        if (st == WIFI_ST_GOT_IP) { last_retries = 0; continue; }
        if (r != last_retries) {          /* 刚发生过一次断线 → 退避后重连 */
            last_retries = r;
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            ESP_LOGI(TAG, "重连 #%u …", (unsigned)r + 1);
            st_lock(); s_st.state = WIFI_ST_CONNECTING; st_unlock();
            esp_wifi_connect();
        }
    }
}

/* ---------------- 对外入口 ---------------- */
void wifi_st_start(void)
{
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = WIFI_ST_STARTING;
    snprintf(s_st.ssid, sizeof(s_st.ssid), "%s", WIFI_CFG_SSID);

    /* 栈 4096 足够：扫描表是堆分配，本任务的栈只放常量与局部小结构 */
    xTaskCreate(wifi_mgr_task, "wifi_mgr", 4096, NULL, 5, NULL);
}
