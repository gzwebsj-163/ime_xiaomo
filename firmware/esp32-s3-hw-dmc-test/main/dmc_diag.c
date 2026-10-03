/**
 * dmc_diag.c — hw_dmc 真机验证桥（ESP32-S3）
 *
 * 两段自检 + 总判，全部打印到 UART0 调试口：
 *
 *   Phase A — 确定性路径（真机 == 宿主逐位一致）
 *     [1] 六模式探测（真机应为 ESP32）
 *     [2] 黄金校验和        HW_DMC_GOLDEN      = 0x169A603E
 *     [3] CRC 标准向量      "123456789"        = 0x29B1
 *     [4] 命令表            9 项 + FNV          = 0x80EEF1A6
 *     [5] 状态表            7 项 + FNV          = 0x9B71FE5F
 *     [6] 帧 FNV                                = 0xB3DFA0D8
 *     [7] 全量自检          fails == 0
 *     [8] 9 命令 pack→unpack 往返（载荷逐位相同）
 *     [9] 默认环回握手      master 收 ACK → ESTABLISHED
 *
 *   Phase B — 真机 UART1 内部回环（帧真的能走线，不需任何外部器件）
 *     [B1] 回环字节自证      {A5 5A 00 FF} 逐位一致
 *     [B2] 帧过 UART         pack→UART→unpack，载荷逐位相同（含 0xAA 0x55 定界）
 *     [B3] 9 命令过 UART     全部往返一致
 *     [B4] 坏帧过 UART       翻 1 位 → unpack 必须 ERR_CRC
 *     [B5] 载荷上界过 UART   249B 载荷（帧 255B / len=0xFF）完整往返
 *          ⚠️ 这条正是历史缺陷的缺口：旧版上限 252→帧 258 装不进 1 字节 LEN，
 *             pack「成功」但结构性无法解出。上界用例必须 round-trip。
 *     [B6] init 保留 BSP     装真机 BSP 后 hw_dmc_init() 不得抹掉它
 *
 * ⚠️ 栈安全：真机 IDLE 栈很小，所有大缓冲一律 static（坑 #12 血泪）。
 */
#include "dmc_diag.h"

#include <stdio.h>
#include <string.h>

#include "dmc_hw.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_dmc.h"

static const char *TAG = "hw_dmc";

/* ============================================================
 * 小工具
 * ============================================================ */

/* selftest 输出回调：只把含 FAIL 的行打出来，其余静默（避免刷屏）。
 * 返回 0 = 无失败。 */
static int dmc_putf(const char *s)
{
    if (s == NULL) return 0;
    if (strstr(s, "FAIL") || strstr(s, "fail") || strstr(s, "MISMATCH")) {
        ESP_LOGE(TAG, "  selftest: %s", s);
    }
    return 0;
}

/* ============================================================
 * Phase A — 确定性路径
 * ============================================================ */
static int phase_a(void)
{
    int fail = 0;

    ESP_LOGI(TAG, "---- Phase A: 确定性路径 (真机 == 宿主逐位一致) ----");

    {   /* [1] 模式 */
        int m = hw_dmc_mode();
        int ok = (m == HW_DMC_MODE_ESP32);
        ESP_LOGI(TAG, "[1] mode=%s (%d) arch=%s %s",
                 hw_dmc_mode_name(m), m, hw_dmc_build_arch(), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [2] 黄金校验和 */
        uint32_t g = (uint32_t)hw_dmc_cmd("golden", NULL);
        int ok = (g == HW_DMC_GOLDEN);
        ESP_LOGI(TAG, "[2] golden=0x%08X (expect 0x%08X) %s",
                 (unsigned)g, (unsigned)HW_DMC_GOLDEN, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [3] CRC 标准向量 */
        uint16_t c = hw_dmc_crc_of_vector();
        int ok = (c == (uint16_t)HW_DMC_CRC_VECTOR);
        ESP_LOGI(TAG, "[3] crc(\"123456789\")=0x%04X (expect 0x%04X) %s",
                 (unsigned)c, (unsigned)HW_DMC_CRC_VECTOR, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [4] 命令表 */
        int cnt = hw_dmc_cmd("cmds", NULL);
        uint32_t fnv = (uint32_t)hw_dmc_cmd("cmdfnv", NULL);
        int ok = (cnt == HW_DMC_CMD_NUM) && (fnv == (uint32_t)HW_DMC_CMD_FNV);
        ESP_LOGI(TAG, "[4] cmds=%d (expect %d) fnv=0x%08X (expect 0x%08X) %s",
                 cnt, HW_DMC_CMD_NUM, (unsigned)fnv, (unsigned)HW_DMC_CMD_FNV,
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [5] 状态表 */
        int cnt = hw_dmc_cmd("states", NULL);
        uint32_t fnv = (uint32_t)hw_dmc_cmd("statefnv", NULL);
        int ok = (cnt == HW_DMC_STATE_NUM) && (fnv == (uint32_t)HW_DMC_STATE_FNV);
        ESP_LOGI(TAG, "[5] states=%d (expect %d) fnv=0x%08X (expect 0x%08X) %s",
                 cnt, HW_DMC_STATE_NUM, (unsigned)fnv, (unsigned)HW_DMC_STATE_FNV,
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [6] 帧 FNV */
        uint32_t fnv = (uint32_t)hw_dmc_cmd("framefnv", NULL);
        int ok = (fnv == (uint32_t)HW_DMC_FRAME_FNV);
        ESP_LOGI(TAG, "[6] framefnv=0x%08X (expect 0x%08X) min=%u max=%u maxpay=%u %s",
                 (unsigned)fnv, (unsigned)HW_DMC_FRAME_FNV,
                 (unsigned)HW_DMC_MIN_FRAME, (unsigned)HW_DMC_MAX_FRAME,
                 (unsigned)HW_DMC_MAX_PAYLOAD, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [7] 全量自检 */
        int f = hw_dmc_selftest(dmc_putf);
        ESP_LOGI(TAG, "[7] selftest fails=%d %s", f, (f == 0) ? "OK" : "FAIL!");
        if (f != 0) fail++;
    }

    {   /* [8] 9 命令 pack→unpack 往返 */
        static const uint8_t cmds[HW_DMC_CMD_NUM] = {
            HW_DMC_CMD_HELLO, HW_DMC_CMD_HELLO_ACK, HW_DMC_CMD_DATA,
            HW_DMC_CMD_DATA_ACK, HW_DMC_CMD_RESET, HW_DMC_CMD_RESET_ACK,
            HW_DMC_CMD_STATUS, HW_DMC_CMD_STATUS_RSP, HW_DMC_CMD_NACK
        };
        static uint8_t pl[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02 };
        static uint8_t buf[HW_DMC_MAX_FRAME];
        int k, ok = 1;
        for (k = 0; k < HW_DMC_CMD_NUM; k++) {
            uint8_t cmd = 0;
            const uint8_t *out = NULL;
            uint16_t olen = 0;
            int n = hw_dmc_pack(cmds[k], pl, 6, buf, (int)sizeof(buf));
            if (n <= 0) { ok = 0; break; }
            int u = hw_dmc_unpack(buf, n, &cmd, &out, &olen);
            if (u != HW_DMC_ERR_OK || cmd != cmds[k] || olen != 6 ||
                memcmp(out, pl, 6) != 0) { ok = 0; break; }
        }
        ESP_LOGI(TAG, "[8] 9-cmd pack/unpack roundtrip %s", ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [9] 默认环回握手（主：发出 HELLO → 收到注入的 HELLO_ACK） */
        static uint8_t ack8[8] = { 0x02, 0x01, 0xFF, 0xFF, 0, 0, 0, 0 };
        static uint8_t buf[HW_DMC_MAX_FRAME];
        hw_dmc_link_t l;
        int n, r;

        hw_dmc_loop_reset();
        /* 先把「对端」的 HELLO_ACK 注入环回（不打本端 tx 标） */
        n = hw_dmc_pack(HW_DMC_CMD_HELLO_ACK, ack8, 8, buf, (int)sizeof(buf));
        if (n > 0) (void)hw_dmc_loop_inject(buf, n);
        hw_dmc_link_init(&l, 1, 2, 100);

        r = hw_dmc_handshake_master(&l, 1);
        int ok = (r == HW_DMC_ERR_OK) && (hw_dmc_link_state(&l) == HW_DMC_ST_ESTABLISHED);
        ESP_LOGI(TAG, "[9] loop handshake rc=%d state=%s %s",
                 r, hw_dmc_state_name(hw_dmc_link_state(&l)), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    ESP_LOGI(TAG, "==== Phase A (确定性) %s ====", (fail == 0) ? "PASS ✔" : "FAIL ✘");
    return fail;
}

/* ============================================================
 * Phase B — 真机 UART1 内部回环
 * ============================================================ */

/* 9 命令表（Phase B 复用） */
static const uint8_t k_cmds[HW_DMC_CMD_NUM] = {
    HW_DMC_CMD_HELLO, HW_DMC_CMD_HELLO_ACK, HW_DMC_CMD_DATA,
    HW_DMC_CMD_DATA_ACK, HW_DMC_CMD_RESET, HW_DMC_CMD_RESET_ACK,
    HW_DMC_CMD_STATUS, HW_DMC_CMD_STATUS_RSP, HW_DMC_CMD_NACK
};

static int phase_b(void)
{
    int fail = 0;
    static uint8_t tx[HW_DMC_MAX_FRAME + 8];
    static uint8_t rx[HW_DMC_MAX_FRAME + 8];
    int i, got, fl;

    ESP_LOGI(TAG, "---- Phase B: 真机 UART%d 内部回环 (TX=IO%d RX=IO%d) ----",
             DMC_UART_PORT, DMC_UART_TX, DMC_UART_RX);

    if (dmc_hw_uart_open() != 0) {
        ESP_LOGE(TAG, "[B0] uart open FAIL");
        return 1;
    }

    {   /* [B1] 回环字节自证（硬，不需外部器件） */
        static const uint8_t pat[4] = { 0xA5, 0x5A, 0x00, 0xFF };
        got = dmc_hw_uart_roundtrip(pat, 4, rx, (int)sizeof(rx), 100);
        int ok = (got == 4) && (memcmp(rx, pat, 4) == 0);
        ESP_LOGI(TAG, "[B1] uart loopback got=%d %02X %02X %02X %02X %s",
                 got, rx[0], rx[1], rx[2], rx[3], ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B2] 一帧过 UART：pack → UART → unpack（含 0xAA/0x55 定界字节） */
        static uint8_t pl[12] = { 0x00, 0xAA, 0x55, 0xFC, 0x11, 0x22,
                                  0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
        uint8_t cmd = 0;
        const uint8_t *out = NULL;
        uint16_t olen = 0;
        int u;

        fl = hw_dmc_pack(HW_DMC_CMD_DATA, pl, 12, tx, (int)sizeof(tx));
        got = dmc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        u = (got == fl) ? hw_dmc_unpack(rx, got, &cmd, &out, &olen) : -1;
        int ok = (got == fl) && (memcmp(rx, tx, (size_t)fl) == 0) &&
                 (u == HW_DMC_ERR_OK) && (cmd == HW_DMC_CMD_DATA) &&
                 (olen == 12) && (memcmp(out, pl, 12) == 0);
        ESP_LOGI(TAG, "[B2] frame over UART pack=%dB got=%d unpack=%d cmd=%s plen=%d %s",
                 fl, got, u, hw_dmc_cmd_name(cmd), (int)olen, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B3] 9 命令全部过 UART 往返 */
        int k, ok = 1;
        for (k = 0; k < HW_DMC_CMD_NUM; k++) {
            uint8_t cmd = 0;
            const uint8_t *out = NULL;
            uint16_t olen = 0;
            static uint8_t pl[4] = { 0x11, 0x22, 0x33, 0x44 };
            fl = hw_dmc_pack(k_cmds[k], pl, 4, tx, (int)sizeof(tx));
            got = dmc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
            if (got != fl || memcmp(rx, tx, (size_t)fl) != 0) { ok = 0; break; }
            if (hw_dmc_unpack(rx, got, &cmd, &out, &olen) != HW_DMC_ERR_OK ||
                cmd != k_cmds[k] || olen != 4 || memcmp(out, pl, 4) != 0) { ok = 0; break; }
        }
        ESP_LOGI(TAG, "[B3] 9-cmd over UART %s", ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B4] 坏帧过 UART：翻 1 位 → 必须 CRC 拒绝 */
        static uint8_t pl[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        uint8_t cmd = 0;
        const uint8_t *out = NULL;
        uint16_t olen = 0;
        int u;

        fl = hw_dmc_pack(HW_DMC_CMD_DATA, pl, 8, tx, (int)sizeof(tx));
        tx[fl - 3] ^= 0xFF;                       /* 翻一个载荷字节（CRC 仍为原值） */
        got = dmc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        int bytes_ok = (got == fl) && (memcmp(rx, tx, (size_t)fl) == 0);  /* 坏字节原样传过去 */
        u = bytes_ok ? hw_dmc_unpack(rx, got, &cmd, &out, &olen) : -1;
        int ok = bytes_ok && (u == HW_DMC_ERR_CRC);
        ESP_LOGI(TAG, "[B4] corrupted frame over UART got=%d unpack=%d (expect ERR_CRC=%d) %s",
                 got, u, HW_DMC_ERR_CRC, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B5] 载荷上界过 UART：249B 载荷（帧 255B, len 字段=0xFF）
         *   ⚠️ 历史缺陷缺口：旧上限 252→帧 258 无法用 1B LEN 表示,
         *      pack 返回「成功」却结构不可解。上界必须 round-trip 才算测过。 */
        static uint8_t pl[HW_DMC_MAX_PAYLOAD];
        uint8_t cmd = 0;
        const uint8_t *out = NULL;
        uint16_t olen = 0;
        int u;

        for (i = 0; i < (int)HW_DMC_MAX_PAYLOAD; i++) pl[i] = (uint8_t)(i & 0xFF);
        fl = hw_dmc_pack(HW_DMC_CMD_DATA, pl, (uint16_t)HW_DMC_MAX_PAYLOAD,
                         tx, (int)sizeof(tx));
        got = dmc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        u = (got == fl) ? hw_dmc_unpack(rx, got, &cmd, &out, &olen) : -1;
        int ok = (fl == (int)HW_DMC_MAX_FRAME) && (got == fl) &&
                 (u == HW_DMC_ERR_OK) && (olen == HW_DMC_MAX_PAYLOAD) &&
                 (memcmp(out, pl, HW_DMC_MAX_PAYLOAD) == 0);
        ESP_LOGI(TAG, "[B5] max payload over UART pack=%dB got=%d plen=%d %s",
                 fl, got, (int)olen, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B6] 关键语义：hw_dmc_init() 必须保留已装的真机 BSP（坑 #25） */
        static uint8_t ack8[8] = { 0x02, 0x01, 0xFF, 0xFF, 0, 0, 0, 0 };
        static uint8_t buf[HW_DMC_MAX_FRAME];
        hw_dmc_link_t l;
        int n, r;

        /* B6a：装真机 BSP → init → 此刻 tx/rx 走 UART（不是环回）。
         * 环里预置一枚 HELLO_ACK，若 BSP 被 init 抹掉，master 会读回它 → OK；
         * 若 BSP 保住了，master 的帧走 UART 只回环到自己（cmd 不匹配）→ 非 OK。
         * ⇒ 断言 r != OK 证明 BSP 存活。 */
        dmc_hw_install(true);
        dmc_hw_uart_flush();
        hw_dmc_loop_reset();
        n = hw_dmc_pack(HW_DMC_CMD_HELLO_ACK, ack8, 8, buf, (int)sizeof(buf));
        if (n > 0) (void)hw_dmc_loop_inject(buf, n);
        hw_dmc_init(NULL);                       /* 上电复位：不得抹 BSP */
        hw_dmc_link_init(&l, 1, 2, 100);
        r = hw_dmc_handshake_master(&l, 1);
        int kept = (r != HW_DMC_ERR_OK);

        /* B6b：卸载回环回 → 同样的环回注入应让 master 成功（证明切换真的生效） */
        dmc_hw_install(false);
        hw_dmc_loop_reset();
        n = hw_dmc_pack(HW_DMC_CMD_HELLO_ACK, ack8, 8, buf, (int)sizeof(buf));
        if (n > 0) (void)hw_dmc_loop_inject(buf, n);
        hw_dmc_link_init(&l, 1, 2, 100);
        r = hw_dmc_handshake_master(&l, 1);
        int back = (r == HW_DMC_ERR_OK);

        int ok = kept && back;
        ESP_LOGI(TAG, "[B6] init keeps BSP: kept=%d back-to-loop=%d %s",
                 kept, back, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    dmc_hw_uart_close();

    ESP_LOGI(TAG, "==== Phase B (真机 UART 传输) %s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘");
    return fail;
}

/* ============================================================
 * 入口
 * ============================================================ */
static void dmc_diag_task(void *arg)
{
    int fa, fb;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_dmc 真机验证开始 (ESP32-S3) ====");
    dmc_hw_dump();

    fa = phase_a();
    fb = phase_b();

    ESP_LOGI(TAG, "==== hw_dmc 真机验证 %s (A=%d B=%d, total=%d) ====",
             (fa + fb == 0) ? "PASS ✔" : "FAIL ✘", fa, fb, fa + fb);

    vTaskDelete(NULL);
}

void dmc_diag_start(void)
{
    xTaskCreate(dmc_diag_task, "hw_dmc", 8192, NULL, 5, NULL);
}
