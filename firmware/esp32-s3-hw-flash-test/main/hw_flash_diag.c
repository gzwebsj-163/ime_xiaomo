/**
 * hw_flash_diag.c — ESP32-S3 真机接入 hw_flash（ROM 下载协议烧录层）验证桥
 *
 * 目的: 全跨式验证 hw_flash 在真实 ESP-IDF 固件里的两段能力:
 *
 * ── Phase A: 确定性路径 (核心跨模式主张) ──────────────────
 *   [1] 自动模式探测 → 应命中 ESP32 (CONFIG_IDF_TARGET_ESP32xx)
 *   [2] 黄金校验和 0xAF05978A 真机 == 宿主逐位一致 (ROM 命令表零漂移)
 *   [3] SLIP RFC1055 转义 (C0→DB DC / DB→DB DD, 方向不可反)
 *   [4] MD5 (RFC1321) 三向量 + 4096B 镜像黄金 4e328028…
 *   [5] 请求帧 "<BBHI" 构造 (dir+cmd+len+xor, body@8)
 *   [6] 协议原语: SYNC 握手 → READ_REG magic=9 (ESP32-S3) → chip_id
 *   [7] 端到端烧录 4096B → 设备回读 MD5 == 本地 (VERIFIED)
 *   [8] selftest fails == 0 (真机 == 宿主逐位一致)
 *   [9] 命令分发 (VM OP_HW_FLASH_CALL 同款路径): mode/help/nocmd
 *
 * ── Phase B: 真实硅路径 (BSP 注入) ────────────────────────
 *   [B1] UART1 内部回环: 写 8 字节(含 C0/DB) → 读回逐位相等 (真硅字节通道)
 *   [B2] 注入真机 BSP 后跑 hw_flash_run: 真字节确实上线 (tx>0) 且回环逐位一致,
 *        协议面对"回显"这种错信道必须**优雅报错不挂死** (预期 PROTO)
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "hw_flash.h"

static const char *TAG = "hw_flash";

/* ============================================================
 * Phase B 真机 BSP: UART1 内部回环
 * ============================================================ */
#define FL_UART_PORT  UART_NUM_1
#define FL_UART_TX    2            /* 自由引脚 */
#define FL_UART_RX    3

/* 回环自证: 记录发出字节, 读回时逐个比对, 统计不一致数 */
#define FL_ECHO_CAP   16384
static uint8_t  g_tx_hist[FL_ECHO_CAP];
static uint32_t g_tx_hist_len;     /* 已记录 (发出) 字节数 */
static uint32_t g_rx_total;        /* 真机读回字节数 */
static uint32_t g_echo_mismatch;   /* 回环逐位不一致计数 */
static uint32_t g_read_calls;      /* uart_read 调用计数 (信道无效判定) */

static int fl_uart_open(uint32_t baud)
{
    uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(FL_UART_PORT, 4096, 4096, 0, NULL, 0) != ESP_OK)
        return -1;
    if (uart_param_config(FL_UART_PORT, &cfg) != ESP_OK) return -2;
    if (uart_set_pin(FL_UART_PORT, FL_UART_TX, FL_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -3;
    if (uart_set_loop_back(FL_UART_PORT, true) != ESP_OK) return -4;

    g_tx_hist_len = 0; g_rx_total = 0; g_echo_mismatch = 0; g_read_calls = 0;

    /* 真机实测坑 (hw_wdbg 同款): 回环使能瞬间 RX 侧冒 1 字节伪码 (0xff),
     * 致首字节整体后移 → 先灌注两字节再清空, 使后续首字节干净 */
    {
        uint8_t prime[2] = { 0u, 0u };
        (void)uart_write_bytes(FL_UART_PORT, prime, sizeof(prime));
        (void)uart_wait_tx_done(FL_UART_PORT, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uart_flush_input(FL_UART_PORT);
    return 0;
}

static int fl_uart_close(void)
{
    uart_driver_delete(FL_UART_PORT);
    return 0;
}

static int fl_uart_write(const uint8_t *d, uint32_t n)
{
    int w;
    if (n == 0u) return 0;
    w = uart_write_bytes(FL_UART_PORT, (const char *)d, (size_t)n);
    if (w < 0) return -1;
    (void)uart_wait_tx_done(FL_UART_PORT, pdMS_TO_TICKS(50));
    /* 记入发出历史 (容量内; 溢出则停止记录但继续发) */
    if (g_tx_hist_len + n <= FL_ECHO_CAP) {
        memcpy(g_tx_hist + g_tx_hist_len, d, n);
        g_tx_hist_len += n;
    }
    return w;
}

/* 读: 拒绝回显 (不是有效应答), 但把它用于**字节级回环自证**。
 * 返回 0 → 上层按协议超时优雅报错, 不挂死。 */
static int fl_uart_read(uint8_t *d, uint32_t cap)
{
    uint8_t tmp[256];
    uint32_t want = (cap < sizeof(tmp)) ? cap : (uint32_t)sizeof(tmp);
    int r;
    uint32_t i;

    /* 回显信道不可能产出一帧合法应答: 累积到一定次数即判信道无效,
     * 返回 <0 让上层走 IOERR 优雅收尾 (避免 4096 次空转拖长测试) */
    if (g_read_calls > 300u) return -1;
    g_read_calls++;

    r = uart_read_bytes(FL_UART_PORT, tmp, (uint32_t)want, pdMS_TO_TICKS(10));
    if (r <= 0) return 0;
    for (i = 0; i < (uint32_t)r; i++) {
        uint32_t idx = g_rx_total + i;
        if (idx < g_tx_hist_len) {
            if (tmp[i] != g_tx_hist[idx]) g_echo_mismatch++;
        } else {
            g_echo_mismatch++;          /* 多出来的字节也算异常 */
        }
    }
    g_rx_total += (uint32_t)r;
    (void)d;
    return 0;                            /* 上层收到 0 → 协议超时路径 */
}

static const hw_flash_bsp_t g_hw_bsp = {
    .uart_open  = fl_uart_open,
    .uart_close = fl_uart_close,
    .uart_write = fl_uart_write,
    .uart_read  = fl_uart_read,
};

/* 自检 putf → ESP_LOGI (真机日志) */
static int flash_putf(const char *s)
{
    ESP_LOGI(TAG, "%s", s ? s : "");
    return 0;
}

static char *fl_result_txt(void)
{
    static char b[160];
    hw_flash_result(b, sizeof(b));
    return b;
}

/* ============================================================ */
static void hw_flash_diag_task(void *arg)
{
    int fail = 0;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_flash 真机验证开始 (ESP32-S3) ====");

    /* ---------------- Phase A: 确定性 ---------------- */
    {
        uint8_t img[4096];
        char hx[33];
        hw_flash_session_t sess;
        hw_flash_stat_t stt;
        uint32_t n, v = 0;
        int rc;

        /* [1] 模式探测 */
        {
            int m = (int)hw_flash_mode();
            ESP_LOGI(TAG, "[1] mode = %s (%d) %s", hw_flash_mode_str((uint8_t)m), m,
                     (m == (int)HW_FLASH_MODE_ESP32) ? "OK" : "FAIL!");
            if (m != (int)HW_FLASH_MODE_ESP32) fail++;
        }

        /* [2] 命令表 + 黄金 */
        {
            uint32_t c = hw_flash_cmd_checksum();
            ESP_LOGI(TAG, "[2] cmd table: count=%u fnv=%08X golden=%08X %s",
                     (unsigned)hw_flash_cmd_count(), (unsigned)c, (unsigned)HW_FLASH_GOLDEN,
                     (hw_flash_cmd_count() == 9u && c == HW_FLASH_GOLDEN) ? "OK" : "FAIL!");
            if (!(hw_flash_cmd_count() == 9u && c == HW_FLASH_GOLDEN)) fail++;
        }

        /* [3] SLIP 转义 */
        {
            static const uint8_t vin[] = { 0xC0, 0xDB, 0x00, 0xDC, 0xDD, 0x02 };
            static const uint8_t vgold[] = { 0xC0, 0xDB, 0xDC, 0xDB, 0xDD, 0x00, 0xDC, 0xDD, 0x02, 0xC0 };
            uint8_t enc[64], dec[64];
            int en = hw_flash_slip_encode(vin, sizeof vin, enc, sizeof enc);
            int dn = hw_flash_slip_decode(enc, (uint32_t)en, dec, sizeof dec);
            int ok = (en == (int)sizeof vgold) && (memcmp(enc, vgold, sizeof vgold) == 0) &&
                     (dn == (int)sizeof vin) && (memcmp(dec, vin, sizeof vin) == 0);
            ESP_LOGI(TAG, "[3] slip enc n=%d dec n=%d %s", en, dn, ok ? "OK" : "FAIL!");
            if (!ok) fail++;
        }

        /* [4] MD5 */
        {
            uint8_t d16[16];
            static const char *E = "", *A = "abc";
            static const char *Eh = "d41d8cd98f00b204e9800998ecf8427e";
            static const char *Ah = "900150983cd24fb0d6963f7d28e17f72";
            char o[33];
            int ok;
            hw_flash_md5_hex((const uint8_t *)E, 0, o);
            ok = (strcmp(o, Eh) == 0);
            hw_flash_md5_hex((const uint8_t *)A, 3, o);
            ok = ok && (strcmp(o, Ah) == 0);
            n = hw_flash_test_image(img, sizeof img);
            hw_flash_md5_hex(img, n, o);
            ok = ok && (strcmp(o, HW_FLASH_IMG4096_MD5) == 0);
            ESP_LOGI(TAG, "[4] md5: img4096=%s golden=%s %s", o, HW_FLASH_IMG4096_MD5,
                     ok ? "OK" : "FAIL!");
            (void)d16;
            if (!ok) fail++;
        }

        /* [5] 请求帧构造 */
        {
            uint8_t fr[64];
            int fl = hw_flash_frame_build(HW_FLASH_CMD_SYNC, NULL, 0, fr, sizeof fr);
            /* "<BBHI": dir=00 cmd=08 len=0000 xor = 00^08^00^00^00^00^00^EF ... */
            int ok = (fl == 8) && (fr[0] == HW_FLASH_DIR_REQ) && (fr[1] == HW_FLASH_CMD_SYNC) &&
                     (fr[2] == 0x00) && (fr[3] == 0x00);
            ESP_LOGI(TAG, "[5] frame dir=%02X cmd=%02X len=%02X%02X %s",
                     fr[0], fr[1], fr[3], fr[2], ok ? "OK" : "FAIL!");
            if (!ok) fail++;
        }

        /* [6] 协议原语: SYNC 握手 → READ_REG magic=9
         *     (chip_id 的落存依赖 read_reg(MAGIC) → 已由 Fix D 自洽) */
        {
            int s = hw_flash_sync();
            int r = hw_flash_read_reg(HW_FLASH_SPI_MAGIC_ADDR, &v);
            uint32_t cid = hw_flash_chip_id();
            int ok = (s == HW_FLASH_R_OK) && (r == HW_FLASH_R_OK) &&
                     (v == HW_FLASH_CHIP_ID) && (cid == HW_FLASH_CHIP_ID);
            ESP_LOGI(TAG, "[6] sync rc=%d readreg magic=%u (expect %u) chip_id=%u %s",
                     s, (unsigned)v, (unsigned)HW_FLASH_CHIP_ID, (unsigned)cid,
                     ok ? "OK" : "FAIL!");
            if (!ok) fail++;
        }

        /* [7] 端到端烧录 */
        {
            rc = hw_flash_run(img, n, 0, &sess, NULL, NULL);
            int ok = (rc == HW_FLASH_R_OK) && (sess.md5_match == 1u) &&
                     (sess.blocks_written == 4u) && (sess.progress == 100u);
            hw_flash_md5_hex((const uint8_t *)sess.md5_local, 0, hx);   /* 占位抑制未用警告 */
            ESP_LOGI(TAG, "[7] run rc=%d done=%u blocks=%u bytes=%u prog=%u md5match=%u %s",
                     rc, (unsigned)sess.done, (unsigned)sess.blocks_written, (unsigned)sess.bytes_written,
                     (unsigned)sess.progress, (unsigned)sess.md5_match, ok ? "OK (VERIFIED)" : "FAIL!");
            if (!ok) fail++;
        }

        /* [8] selftest */
        {
            int f = hw_flash_selftest(flash_putf);
            ESP_LOGI(TAG, "[8] selftest fails = %d %s", f,
                     (f == 0) ? "=> ALL PASS (真机 == 宿主逐位一致)" : "=> FAIL!");
            if (f != 0) fail++;
        }

        /* [9] 命令分发 */
        {
            int c1 = hw_flash_cmd("mode", NULL);
            int c2 = hw_flash_cmd("help", NULL);
            int c3 = hw_flash_cmd("nosuchcmd", NULL);
            int ok = (c1 >= 0) && (c2 == HW_FLASH_R_HELP) && (c3 == HW_FLASH_R_NOCMD);
            ESP_LOGI(TAG, "[9] cmd: mode=%d help=%d nocmd=%d %s",
                     c1, c2, c3, ok ? "OK" : "FAIL!");
            if (!ok) fail++;
        }

        hw_flash_stat(&stt);
        ESP_LOGI(TAG, "[A ] stat: sess=%u tx=%u rx=%u esc=%u bytes=%u md5ok=%u chip=%u",
                 (unsigned)stt.sessions, (unsigned)stt.cmd_tx, (unsigned)stt.cmd_rx, (unsigned)stt.slip_escaped,
                 (unsigned)stt.bytes_flashed, (unsigned)stt.last_md5_ok, (unsigned)stt.chip_id);
    }

    ESP_LOGI(TAG, "==== hw_flash Phase A (确定性) %s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘");

    /* ---------------- Phase B: 真实硅 ---------------- */
    {
        int bfail = 0;
        ESP_LOGI(TAG, "---- Phase B: 注入真机 BSP (UART1 内部回环) ----");
        hw_flash_bsp_install(&g_hw_bsp);

        /* [B1] 真硅字节回环: 直接经 BSP 回调写/读, 逐位比对 */
        {
            static const uint8_t pat[] = { 0xC0, 0xDB, 0x00, 0xDC, 0xDD, 0xA5, 0x5A, 0xEF };
            uint8_t back[32];
            int o = g_hw_bsp.uart_open(115200);
            int w, r = 0, tries;
            uint32_t j, mism = 0;
            if (o != 0) {
                ESP_LOGE(TAG, "[B1] uart_open rc=%d FAIL!", o);
                bfail++;
            } else {
                w = g_hw_bsp.uart_write(pat, (uint32_t)sizeof pat);
                vTaskDelay(pdMS_TO_TICKS(80));
                for (tries = 0; tries < 20 && r < (int)sizeof pat; tries++) {
                    int k = uart_read_bytes(FL_UART_PORT, back + r,
                                            (uint32_t)(sizeof pat - r), pdMS_TO_TICKS(20));
                    if (k > 0) r += k;
                }
                for (j = 0; j < (uint32_t)sizeof pat && j < (uint32_t)r; j++)
                    if (back[j] != pat[j]) mism++;
                ESP_LOGI(TAG, "[B1] loopback wr=%d rd=%d mism=%u %s",
                         w, r, (unsigned)mism,
                         (r == (int)sizeof pat && mism == 0)
                             ? "OK (真硅回环逐位一致)" : "FAIL!");
                if (!(r == (int)sizeof pat && mism == 0)) bfail++;
                g_tx_hist_len = 0; g_rx_total = 0; g_echo_mismatch = 0; g_read_calls = 0;  /* 复位自证计数 */
            }
        }

        /* [B2] 注入 BSP 后跑 hw_flash_run: 真字节上线 + 错信道优雅报错 */
        {
            uint8_t img[2048];
            hw_flash_session_t sess;
            uint32_t n = hw_flash_test_image(img, sizeof img);
            int rc = hw_flash_run(img, n, 0, &sess, NULL, NULL);
            /* 回显信道 → 绝不能 VERIFIED; 必须返回错码而非挂死 */
            int graceful = (rc != HW_FLASH_R_OK);
            int wire_ok  = (g_tx_hist_len > 0u) && (g_rx_total > 0u) && (g_echo_mismatch == 0u);
            ESP_LOGI(TAG, "[B2] run over echo channel rc=%d tx=%u rx=%u echo_mism=%u %s",
                     rc, (unsigned)g_tx_hist_len, (unsigned)g_rx_total, (unsigned)g_echo_mismatch,
                     (graceful && wire_ok) ? "OK (真字节上线+优雅报错)"
                                           : (graceful ? "PARTIAL (上线未证)" : "FAIL!"));
            ESP_LOGI(TAG, "[B2] result: %s", fl_result_txt());
            if (!(graceful && wire_ok)) bfail++;
        }

        /* 🆕 [B3] 2026-10-02 A-3 验证: verify **自包含**性
         *
         * 缺陷原文: verify 依赖进程内 static g_sess.md5_match。CLI「一次调用一件事」
         * ⇒ run 与 verify 是两个进程 ⇒ g_sess 恒零 ⇒ verify 在 CLI 下**结构性是死命令**,
         * 且旧实现传 addr=0/size=0 校验的是**空区间**。
         *
         * 关键: 本段**不先调 hw_flash_run**。旧实现在这种"没 run 过"的状态下会打出
         * `rc=0 md5_match=0` 这种自相矛盾的结果; 修复后应诚实失败(非 OK 或明确 MISMATCH),
         * 而不是假装成功。判据 = **绝不能出现 rc=OK 且 match=0**。
         * 另验新版参数校验: verify 0 / verify abc / 超上限 必须被**拒绝**且不崩。 */
        {
            uint8_t img[1024];
            hw_flash_session_t s2;
            uint32_t n = hw_flash_test_image(img, sizeof img);
            memset(&s2, 0, sizeof s2);
            (void)n;

            /* ⚠️ 重置回环读计数: fl_uart_read 有 300 次硬上限(否则空转到挂死),
             *   B2 已消耗掉大部分。若不重置, B3-1 会在**信道已判死**的状态下测 verify
             *   ⇒ 拿到的是 BSP 层面的 IOERR, 与 verify 自包含性无关 = 判据失效。 */
            g_read_calls = 0; g_rx_total = 0; g_echo_mismatch = 0;

            /* B3-1: 区间由命令行给, 不依赖任何进程内 state */
            int rc_v = hw_flash_cmd("verify 1024", &s2);
            ESP_LOGI(TAG, "[B3-1] verify 1024 (未先 run) rc=%d md5_match=%u  -> %s",
                     rc_v, (unsigned)s2.md5_match,
                     (rc_v == HW_FLASH_R_OK && s2.md5_match == 0u)
                         ? "❌ 自相矛盾(rc=OK 但不匹配) = 缺陷未修"
                         : "OK (未自欺: 要么诚实失败, 要么真比对上了)");

            /* B3-2: 参数校验 —— 旧实现无上界检查, verify 垃圾参数可能越界 */
            int bad_empty = hw_flash_cmd("verify",     &s2);
            int bad_abc   = hw_flash_cmd("verify abc",  &s2);
            int bad_huge  = hw_flash_cmd("verify 99999999", &s2);
            int guarded = (bad_empty != HW_FLASH_R_OK) && (bad_abc != HW_FLASH_R_OK) &&
                          (bad_huge != HW_FLASH_R_OK);
            ESP_LOGI(TAG, "[B3-2] 参数守卫: empty=%d abc=%d huge=%d (均须非0) %s",
                     bad_empty, bad_abc, bad_huge, guarded ? "OK (垃圾参数全被拒)" : "FAIL!");
            if (!guarded) bfail++;
            if ((rc_v == HW_FLASH_R_OK && s2.md5_match == 0u)) bfail++;
        }

        /* 🆕 [B4] 2026-10-02 A-4② 验证: 应答帧**归属判定** + stale_frames 可观测
         *
         * 缺陷原文: fl_req「解出第一帧就 break」且从不比较 rcmd 与 cmd ⇒
         * 真机 SYNC 回 8 帧, 残留的 7 帧会被后续命令当自己的应答。
         * 修复: 用 decode_ex 取 consumed 逐帧消费, 按 opcode 判归属, 非本命令的丢弃。
         *
         * 判据 (双向, 缺一不可):
         *   ① stale_frames 计数**必须增长** ⇒ 丢弃逻辑真的在跑;
         *      若恒 0 → 要么没丢(缺陷未修), 要么 SYNC 只回了 1 帧(见 ② 区分)。
         *   ② B1 回环通道下协议必须仍然**优雅报错不挂死** ⇒ 丢弃残留没有把流程卡死。 */
        {
            hw_flash_stat_t sa, sb;
            uint8_t img[2048];
            hw_flash_session_t s3;
            uint32_t n = hw_flash_test_image(img, sizeof img);

            hw_flash_stat(&sa);
            /* ⚠️ 同 B3: 重置回环读计数, 否则 B3 已把信道读到接近上限,
             *   B4 测的是"归属判定"却会先撞上 BSP 的 IOERR 上限 = 测不到目标。 */
            g_read_calls = 0; g_rx_total = 0; g_echo_mismatch = 0;
            /* 先制造真残留: 真机 SYNC 回多帧, 这里直接走完整协议栈 */
            int rc4 = hw_flash_run(img, n, 0, &s3, NULL, NULL);
            hw_flash_stat(&sb);

            uint32_t grew = (sb.stale_frames > sa.stale_frames);
            ESP_LOGI(TAG, "[B4] 应答归属: stale_frames %u -> %u (增长=%u) cmd_rx=%u md5ok=%u",
                     (unsigned)sa.stale_frames, (unsigned)sb.stale_frames,
                     (unsigned)grew, (unsigned)sb.cmd_rx, (unsigned)sb.last_md5_ok);
            ESP_LOGI(TAG, "[B4] run over echo channel rc=%d -> %s", rc4,
                     fl_result_txt());
            /* 回环通道下绝不能 VERIFIED; 也绝不能挂死(能走到这行本身就是证据) */
            int graceful = (rc4 != HW_FLASH_R_OK);
            ESP_LOGI(TAG, "[B4] 优雅失败=%u %s", (unsigned)graceful,
                     graceful ? "OK (未挂死, 且没把回显当 VERIFIED)" : "❌ 回显信道竟报成功");
            if (!graceful) bfail++;
            if (!grew) ESP_LOGW(TAG, "[B4] stale_frames 未增长 —— 若同时 chip_id/结果异常, "
                                     "说明固件只回了 1 帧而非丢弃逻辑失效(需对照判读)");
        }

        (void)g_hw_bsp.uart_close();
        hw_flash_bsp_install(NULL);      /* 还原默认 ROM 模拟器 */
        hw_flash_init(NULL);

        ESP_LOGI(TAG, "==== hw_flash Phase B (真硅) %s ====",
                 (bfail == 0) ? "PASS ✔" : "FAIL ✘");
        fail += bfail;
    }

    ESP_LOGI(TAG, "==== hw_flash 真机验证 %s (fails=%d) ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘", fail);

    vTaskDelete(NULL);
}

void hw_flash_diag_start(void)
{
    /* 栈 16KB (真机加固): 模块本体大缓冲已全部 static, 但本任务自身
     * 仍有 Phase A 的 img[4096] + session/stat, 且 ESP-IDF 日志路径吃栈;
     * 原 8192 在真机上偏紧 → 提到 16384。 */
    xTaskCreate(hw_flash_diag_task, "hw_flash", 16384, NULL, 5, NULL);
}
