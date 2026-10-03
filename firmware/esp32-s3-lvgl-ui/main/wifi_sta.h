/**
 * wifi_sta.h — 真实 WiFi STA 连接层（2026-09-29 新增）
 *
 * 之前的 STATUS 页 WiFi 是**假数据**（硬编码 IP + 3s 轮换假 IP 池）。
 * 本模块把它换成真实的：上电 → 扫描 → 连接 → 拿真 IP / RSSI → 掉线自动重连。
 *
 * 设计要点：
 *   · **异步非阻塞**：wifi_st_start() 立刻返回，连接/扫描/重连全在后台任务里跑，
 *     不拖慢 UI 启动，也不在事件回调里做重活（避免饿死 IDLE 触发看门狗）。
 *   · **快照读**：UI 线程只调 wifi_st_get() 拿一份拷贝（带互斥保护），
 *     绝不从 LVGL 任务里碰 WiFi API（LVGL 非线程安全，WiFi 回调也非 LVGL 上下文）。
 *   · **凭据优先 NVS**：首次开机用 wifi_cfg.h 里的默认值，连上后写进 NVS，
 *     以后以 NVS 为准 —— 为后续「屏上配网」留好接口。
 *   · **原因码可读**：断线把 esp_wifi 的 reason 翻译成人话打日志 + 显示，
 *     4000 系列/201/15 这些一眼能看出是「找不到 AP」还是「密码错」。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_ST_OFF = 0,     /* 未启动 */
    WIFI_ST_STARTING,    /* 驱动初始化中 */
    WIFI_ST_SCANNING,    /* 扫描附近 AP（启动诊断） */
    WIFI_ST_CONNECTING,  /* 已下发 connect，等 IP */
    WIFI_ST_GOT_IP,      /* 已连上并拿到 IP */
    WIFI_ST_FAIL,        /* 反复失败，等待下一次重试 */
} wifi_st_state_t;

typedef struct {
    wifi_st_state_t state;
    char     ip[16];      /* 拿到 IP 才有内容，如 "192.168.0.173" */
    char     ssid[33];    /* 目标 SSID（来自 NVS 或默认值） */
    int      rssi;        /* dBm；0 = 未知 */
    int      channel;     /* 当前 AP 信道 */
    int      last_reason; /* 最近一次 disconnect 的 reason 码（0 = 无） */
    uint32_t retries;     /* 累计重连次数（连上后清零） */
    bool     scan_done;   /* 启动扫描是否已完成（未完成时 UI 不该报“找不到 AP”） */
    bool     ap_seen;     /* 定向扫描时，目标 SSID 是否可见 */
    int      scan_n;      /* 全量扫描到的 AP 总数 */
} wifi_st_status_t;

/** 启动 WiFi STA（非阻塞）。内部自己初始化 NVS / netif / event loop。 */
void wifi_st_start(void);

/** 取状态快照（线程安全，可在 LVGL 任务里调用）。out 不能为 NULL。 */
void wifi_st_get(wifi_st_status_t *out);

/** 把 esp_wifi disconnect reason 码翻译成短字符串（用于日志/显示）。 */
const char *wifi_st_reason_str(int reason);

#ifdef __cplusplus
}
#endif
