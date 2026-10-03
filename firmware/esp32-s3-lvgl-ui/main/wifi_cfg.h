/**
 * wifi_cfg.h — WiFi 默认凭据（首次开机的种子值）
 *
 * ⚠️ 只在「NVS 里还没有凭据」时生效。连上成功后凭据会写进 NVS，
 *    之后以 NVS 为准（改这里的值需要先擦 NVS 或从屏上重新配网）。
 *
 * 🚨 硬件硬约束：**ESP32-S3 是 2.4GHz only，物理上没有 5GHz 射频**。
 *    目标 SSID 名字里带 "_5g" —— 如果那个 AP 是纯 5GHz 的，
 *    芯片**扫描阶段就完全看不到它**（不是密码问题，是射频不存在）。
 *    日志里会打印扫描到的 AP 列表：若目标 SSID 不在列表里，
 *    就说明它是 5GHz（或太远/隐藏），需要换连 2.4GHz 那个 SSID。
 */
#pragma once

/* 默认 SSID / 密码
 *
 * ⚠️ 2026-09-29 真机扫描修正：用户最初给的是 "FYX_E809_5g" ——
 *    实测定向扫描 0 命中、连接 reason=201(NO_AP_FOUND) 连续 7 次。
 *    全量扫描发现同一个路由器还广播 **"FYX_E809_2.4g"**（-38dBm, CH1, WPA/WPA2-PSK），
 *    所以原来是连错了频段的 SSID。**ESP32-S3 只有 2.4GHz 射频，5GHz AP 物理上不可见**。
 */
#define WIFI_CFG_SSID   "FYX_E809_2.4g"
#define WIFI_CFG_PASS   "34023077aa.."

/* 🔒 入库纪律（2026-10-02 记录）
 *
 *   上面是**明文密码**。本工程目录 `esp32-s3-lvgl-ui/` **不在任何 git 仓库内**
 *   （已用 `git rev-parse --show-toplevel` 核实为「不在仓库」），因此暂无泄露风险。
 *
 *   ⚠️ 如果将来要把这个工程纳入 git（例如并入 ime_xiaomo 主仓库），必须先做其一：
 *      ① 把本文件加入 .gitignore，改由构建期注入（如 sdkconfig.defaults + 密钥管理）
 *      ② 或把凭据改为 NVS 首次配网写入，删除这里的种子值
 *   切勿直接 `git add .` —— 明文口令会直接进历史，事后删除无效。
 */
