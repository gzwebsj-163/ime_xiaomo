/**
 * dc_diag.c — ESP32-S3 真机接入 hw_dc（DC 电源信号层）验证桥
 *
 * ── Phase A: 确定性路径 (核心跨模式主张, 不需要任何硬件) ──────
 *   [1] 自动模式探测 → 命中 ESP32 (CONFIG_IDF_TARGET_ESP32S3)
 *   [2] 黄金校验和 0x028EECE6 真机 == 宿主逐位一致
 *   [3] 信号表 8 路 / [4] 组合数据帧 7 项 (DC_DATA_5=0x0D9)
 *   [5] 参考预值 IN 0x059 / OUT 0x080
 *   [6] selftest fails == 0 (真机 == 宿主逐位一致)
 *   [7] 命令分发 (VM OP_HW_DC_CALL 同款路径)
 *   [8] 用户原槽位 API: dc_create_mode / dc_init / dc_mode_input / dc_ready
 *
 * ── Phase B: 真实硅路径 (BSP 注入) ────────────────────────
 *   [B1] **数字通路自证 (硬, 不需外部器件)**: 内部上拉/下拉让 HILITE/LOLITE
 *        各跳变一次, 4 个期望值全中 —— 引脚真的能读到电平变化。
 *   [B2] ADC 存活体检 (软): 连采 16 次全在量程内。
 *   [B3] **ADC 硬自证 (需一根跳线 PROBE↔ADC_IN)**: PROBE 是纯数字脚(未被
 *        ADC 占用→驱动有效), 驱动高/低各读 ADC。短接则分离度 >1000mV。
 *        ⚠️ 无跳线 = SKIP, **不算失败, 也绝不用噪声漂移冒充通过** ——
 *        原因见 dc_hw.h 文件头 ② (adc_oneshot 会独占 pad 关掉本脚输出,
 *        故「本脚自驱动」在结构上不可能成立)。
 *   [B4] **走模块路径对拍 (硬, 反空转)**: hw_dc_sig_value(DC_INPUT) == 直读
 *        ADC mV; DC_HILITE/LOLITE == 引脚电平 → 证明注入真机 BSP 后
 *        模块**真的在读硬件**而非照常返回表值 (坑#17)。
 *   [B5] 诚实失败 (硬): 注入"读失败"BSP → sig_value == -1 且 in_range == 0
 *   [B6] 上电不静默卸载 (硬): 装 BSP → hw_dc_init (kvm_run 上电会同款调用)
 *        → BSP 必须仍在。真机陷阱: 若 init 抹掉绑定, HUD 显示模拟值却
 *        "看着一切正常" (典型假成功)。
 *   [B7] 卸载 → SIM: NULL = SIM/REAL 分水岭成立
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw_dc.h"
#include "dc_hw.h"

static const char *TAG = "hw_dc";

#define DC_GOLDEN_EXPECT 0x028EECE6u

static int dc_putf(const char *s)
{
    ESP_LOGI(TAG, "  %s", s ? s : "(null)");
    return 0;
}

/* ---- 自证用的 BSP 桩 ---- */
static int neg_read(int sig)  { (void)sig; return -1; }        /* 永远读失败 */
static int sent_read(int sig) { (void)sig; return 0xABCD; }    /* 哨兵值 */

static const hw_dc_bsp_t s_bsp_neg      = { neg_read,  NULL };
static const hw_dc_bsp_t s_bsp_sentinel = { sent_read, NULL };

/* ============================================================
 * Phase A
 * ============================================================ */
static int phase_a(void)
{
    int fail = 0;

    hw_dc_init(NULL);        /* 干净起点（此刻无 BSP → SIM）*/

    {   /* [1] 模式探测 */
        uint8_t m = hw_dc_mode();
        int ok = (m == (uint8_t)HW_DC_MODE_ESP32);
        ESP_LOGI(TAG, "[1] mode=%s (%u) %s", hw_dc_mode_str(m), (unsigned)m,
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [2] 黄金校验和 */
        uint32_t ck = hw_dc_checksum();
        int ok = (ck == DC_GOLDEN_EXPECT);
        ESP_LOGI(TAG, "[2] golden=0x%08X (expect 0x%08X) %s",
                 (unsigned)ck, (unsigned)DC_GOLDEN_EXPECT, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [3] 信号表 */
        int ok = (hw_dc_sig_count() == 8u) &&
                 (strcmp(hw_dc_sig_name(DC_INPUT),  "INPUT")  == 0) &&
                 (strcmp(hw_dc_sig_name(DC_LOW_LITE), "LOLITE") == 0) &&
                 (strcmp(hw_dc_side_name(DC_SIDE_IN), "IN") == 0) &&
                 (strcmp(hw_dc_side_name(DC_SIDE_OUT), "OUT") == 0);
        ESP_LOGI(TAG, "[3] sig table count=%u names OK=%d %s",
                 (unsigned)hw_dc_sig_count(), ok, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [4] 组合数据帧 */
        int ok = (hw_dc_data_count() == 7u) &&
                 (hw_dc_data(4) == (uint32_t)DC_DATA_5) &&
                 ((uint32_t)DC_DATA_5 == 0x0D9u) &&
                 (hw_dc_data(3) == 0x003u);
        ESP_LOGI(TAG, "[4] data frames count=%u DATA_5=0x%03X %s",
                 (unsigned)hw_dc_data_count(), (unsigned)hw_dc_data(4),
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [5] 参考预值 */
        int ok = (hw_dc_base_signal(DC_SIDE_IN) == 0x059) &&
                 (hw_dc_base_signal(DC_SIDE_OUT) == 0x080);
        ESP_LOGI(TAG, "[5] base IN=0x%03X OUT=0x%03X %s",
                 (unsigned)hw_dc_base_signal(DC_SIDE_IN),
                 (unsigned)hw_dc_base_signal(DC_SIDE_OUT), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [6] selftest */
        int f = hw_dc_selftest(dc_putf);
        ESP_LOGI(TAG, "[6] selftest fails=%d %s", f,
                 (f == 0) ? "=> ALL PASS (真机 == 宿主逐位一致)" : "=> FAIL!");
        if (f != 0) fail++;
    }
    {   /* [7] 命令分发 */
        int c1 = hw_dc_cmd("count", NULL);
        int c2 = hw_dc_cmd("ok", NULL);
        int c3 = hw_dc_cmd("data 4", NULL);
        int c4 = hw_dc_cmd("base 0", NULL);
        int c5 = hw_dc_cmd("bogus", NULL);
        int c6 = hw_dc_cmd("help", NULL);
        int ok = (c1 == 8) && (c2 == 1) && (c3 == (int)DC_DATA_5) &&
                 (c4 == 0x059) && (c5 == -1) && (c6 == -2);
        ESP_LOGI(TAG, "[7] cmd count=%d ok=%d data4=0x%03X base0=0x%03X bogus=%d help=%d %s",
                 c1, c2, (unsigned)c3, (unsigned)c4, c5, c6, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [8] 用户原槽位 API */
        int args[DC_SIG_COUNT] = { 11, 22, 44, 33, 55, 66, 77, 88 };
        int handler[DC_SIG_COUNT];
        int frame[1] = { 0 };
        int off = DC_INPUT;
        uint32_t i;
        int ok = 1;

        dc_create_mode(args);
        if (hw_dc_sig_value(DC_OUTPUT) != 22) ok = 0;
        if (!dc_mode_input() || dc_mode_input()[7] != 88) ok = 0;
        dc_init(handler, frame);
        for (i = 0; i < (uint32_t)DC_SIG_COUNT; i++) if (handler[i] != args[i]) ok = 0;
        if (frame[0] != (int)DC_DATA_5) ok = 0;
        if (dc_ready(handler, &off) != 0) ok = 0;                     /* INPUT(11) 越 [33,44] */
        off = DC_MAXIN;  if (dc_ready(handler, &off) != 1) ok = 0;    /* 44 命中 */
        if (dc_ready(NULL, &off) != -1) ok = 0;                       /* NULL 容错 */
        ESP_LOGI(TAG, "[8] slot APIs create_mode/init/mode_input/ready %s",
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    hw_dc_init(NULL);
    ESP_LOGI(TAG, "==== hw_dc Phase A (确定性) %s ====", (fail == 0) ? "PASS ✔" : "FAIL ✘");
    return fail;
}

/* ============================================================
 * Phase B
 * ============================================================ */
static int phase_b(void)
{
    int fail = 0, rc, jumper = 1;   /* jumper: 1 = 未接跳线(SKIP) */

    ESP_LOGI(TAG, "---- Phase B: 注入真机 BSP (片上 ADC + GPIO) ----");

    rc = dc_hw_install(true);
    if (rc != 0) {
        ESP_LOGE(TAG, "[B0] dc_hw_install 失败 rc=%d → 无法进入真实硅路径", rc);
        return 1;
    }
    dc_hw_dump();

    {   /* [B1] 数字通路自证（硬证据，不需外部器件）*/
        int a = -1, b = -1, c = -1, d = -1;
        int r = dc_hw_prove_digital(&a, &b, &c, &d);
        int ok = (r == 0);
        ESP_LOGI(TAG, "[B1] digital prove  HILITE pd/pu=%d/%d  LOLITE pu/pd=%d/%d %s",
                 a, b, c, d, ok ? "OK (引脚真能跳变, 硬证据)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B2] ADC 存活体检（软证据）*/
        int vmin = -1, vmax = -1, vavg = -1;
        int r = dc_hw_adc_sane(&vmin, &vmax, &vavg);
        int ok = (r == 0);
        ESP_LOGI(TAG, "[B2] adc sane  min/max/avg = %d/%d/%d mV %s",
                 vmin, vmax, vavg, ok ? "OK (ADC 在采样且量程内)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B3] ADC 硬自证（需跳线 PROBE ↔ ADC_IN）*/
        int vh = -1, vl = -1;
        int r = dc_hw_probe_loopback(&vh, &vl);
        if (r == 0) {
            jumper = 0;
            ESP_LOGI(TAG, "[B3] adc hard-prove  PROBE hi/lo=%d/%d mV %s",
                     vh, vl, "OK (跳线在位 → ADC 读数正确性成立, 硬证据)");
        } else if (r == 1) {
            ESP_LOGW(TAG, "[B3] adc hard-prove  PROBE hi/lo=%d/%d mV SKIP (无跳线, 非失败)",
                     vh, vl);
            ESP_LOGW(TAG, "      → 想要 ADC 硬证据: 用杜邦线短接 IO%d(PROBE) ↔ IO%d(ADC_IN) 再跑",
                     DC_PIN_PROBE, DC_PIN_ADC_IN);
        } else {
            ESP_LOGW(TAG, "[B3] adc hard-prove rc=%d SKIP (读数失败)", r);
        }
    }

    {   /* [B4] 走模块路径对拍（反空转关键）*/
        int mv_direct = dc_hw_adc_mv(DC_SIDE_IN);
        int mv_module = hw_dc_sig_value(DC_INPUT);
        int d_hi = hw_dc_sig_value(DC_HIGH_LITE);
        int d_lo = hw_dc_sig_value(DC_LOW_LITE);
        int diff = mv_module - mv_direct;
        if (diff < 0) diff = -diff;
        int ok = (mv_direct >= 0) && (diff <= 64) &&
                 (d_hi == 0 || d_hi == 1) && (d_lo == 0 || d_lo == 1);
        ESP_LOGI(TAG, "[B4] module==hardware  INPUT %d mV (direct %d, diff %d) HILITE=%d LOLITE=%d %s",
                 mv_module, mv_direct, diff, d_hi, d_lo, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B5] 诚实失败 */
        hw_dc_bsp_install(&s_bsp_neg);
        int v = hw_dc_sig_value(DC_INPUT);
        int inr = hw_dc_in_range(DC_INPUT);
        int ok = (v == -1) && (inr == 0);
        ESP_LOGI(TAG, "[B5] honest-fail  read-fail → sig_value=%d in_range=%d %s",
                 v, inr, ok ? "OK (不假装读到/不假装在窗口内)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B6] 上电不静默卸载（kvm_run 上电会同款调用 hw_dc_init）*/
        hw_dc_bsp_install(&s_bsp_sentinel);
        int before = hw_dc_sig_value(DC_INPUT);          /* 0xABCD */
        hw_dc_init(NULL);                                /* ← 模拟 VM 上电 */
        int after = hw_dc_sig_value(DC_INPUT);
        int ok = (before == 0xABCD) && (after == 0xABCD);
        ESP_LOGI(TAG, "[B6] init-keeps-bsp  before=0x%X after=0x%X %s",
                 before, after,
                 ok ? "OK (上电不静默卸载硬件绑定)" : "FAIL! (BSP 被 init 清零)");
        if (!ok) fail++;
    }

    /* 还原真机 BSP，做实时读数观察（示波性证据：读数会随手指/环境变化）*/
    hw_dc_bsp_install(NULL);
    dc_hw_install(true);
    {
        char line[160];
        int i;
        ESP_LOGI(TAG, "[B*] 实时读数观察 (IN/OUT mV, 共 5 次):");
        for (i = 0; i < 5; i++) {
            snprintf(line, sizeof(line), "      t=%lldms  IN=%d OUT=%d  HILITE=%d LOLITE=%d",
                     (long long)(esp_timer_get_time() / 1000),
                     hw_dc_sig_value(DC_INPUT), hw_dc_sig_value(DC_OUTPUT),
                     hw_dc_sig_value(DC_HIGH_LITE), hw_dc_sig_value(DC_LOW_LITE));
            ESP_LOGI(TAG, "%s", line);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    {   /* [B7] 卸载 → SIM */
        dc_hw_install(false);
        dc_hw_hw_deinit();
        hw_dc_init(NULL);
        int v = hw_dc_sig_value(DC_INPUT);
        int ok = (v == (int)DC_MACRO_INPUT);
        ESP_LOGI(TAG, "[B7] uninstall → SIM  sig_value=%d (expect %d) %s",
                 v, (int)DC_MACRO_INPUT, ok ? "OK (NULL = SIM 分水岭成立)" : "FAIL!");
        if (!ok) fail++;
    }

    ESP_LOGI(TAG, "==== hw_dc Phase B (真实硅) %s%s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘",
             (fail == 0 && jumper) ? "  (ADC 硬证待跳线: SKIP)" : "");
    return fail;
}

/* ============================================================
 * Phase C: DCPP 帧层真机传输（UART1 内部回环，不需外部器件）
 *
 *   信号层 (Phase B) 证明「引脚真的能读」；本段证明「帧真的能走线」。
 *   帧的逐字节状态机在宿主内存里过了，但字节经过**真实 UART 外设**
 *   (FIFO / 波特率 / 采样 / 定界) 后是否逐位一致，只有真机能证。
 *
 *   [C1] 回环字节自证 (硬): 写 4 个已知字节 → 回读必须逐位相同
 *        (同时验证伪码已被喂掉 —— hw_wdbg 首字节 0xff 坑的回归护栏)
 *   [C2] DCPP PING 帧过 UART (硬): 打帧→写→回读→逐字节相同→feed 成帧
 *        → 应答必须 == 0x50 (PONG)。「帧结构在真实传输上无损」的核心证据
 *   [C3] 会话一条龙过 UART (硬): PING/COUNT/OPEN/SET/GET/RANGE/DATA/BASE/CLOSE
 *        9 帧逐帧走 UART → 成帧数=9 / err=0 / last=CLOSE
 *   [C4] 损坏帧过 UART 被拒 (硬, 反假成功): 翻转 CKSUM 字节后过 UART
 *        → 必须 HW_DCPP_ERR_CKSUM。证明完整性校验在真实传输上仍生效
 *   [C5] 垃圾后重同步 (硬): 先灌 3 个垃圾字节 → 再发合法 PING 帧
 *        → 必须恢复正常成帧并应答 PONG (状态机抗错能力)
 * ============================================================ */

/* 按命令构造规范帧（负载约定与模块 selftest 的会话一条龙一致） */
static int dc_build_cmd(uint8_t cmd, uint8_t* out, int cap)
{
    uint8_t pl[3];
    switch (cmd) {
    case DCPP_CMD_GET:
    case DCPP_CMD_RANGE:  pl[0] = 0;                    return hw_dc_proto_build(cmd, 0, pl, 1, out, cap);
    case DCPP_CMD_BASE:   pl[0] = (uint8_t)DC_SIDE_OUT; return hw_dc_proto_build(cmd, 0, pl, 1, out, cap);
    case DCPP_CMD_DATA:   pl[0] = 4;                    return hw_dc_proto_build(cmd, 0, pl, 1, out, cap);
    case DCPP_CMD_SET:    pl[0] = 0; pl[1] = 42; pl[2] = 0; return hw_dc_proto_build(cmd, 0, pl, 3, out, cap);
    default:              return hw_dc_proto_build(cmd, 0, NULL, 0, out, cap);
    }
}

static int phase_c(void)
{
    int fail = 0;
    uint8_t tx[HW_DCPP_MAX_FRAME];
    uint8_t rx[HW_DCPP_MAX_FRAME];
    int i, k, fl, got, dec;

    if (dc_hw_uart_open() != 0) {
        ESP_LOGE(TAG, "[C0] uart open FAIL (UART%d 内部回环)", DC_UART_PORT);
        return 1;
    }
    ESP_LOGI(TAG, "---- Phase C: DCPP 帧层真机传输 (UART%d TX=IO%d RX=IO%d 回环) ----",
             DC_UART_PORT, DC_UART_TX, DC_UART_RX);

    {   /* [C1] 回环字节自证（硬，不需外部器件）*/
        static const uint8_t pat[4] = { 0xA5, 0x5A, 0x00, 0xFF };
        got = dc_hw_uart_roundtrip(pat, 4, rx, (int)sizeof(rx), 100);
        int ok = (got == 4) && (memcmp(rx, pat, 4) == 0);
        ESP_LOGI(TAG, "[C1] uart loopback  got=%d %02X %02X %02X %02X %s",
                 got, rx[0], rx[1], rx[2], rx[3], ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [C2] DCPP PING 帧过 UART → 成帧 → PONG */
        hw_dc_init(NULL);
        hw_dc_proto_reset();
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, tx, (int)sizeof(tx));
        got = dc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        int ok = (fl > 0) && (got == fl) && (memcmp(rx, tx, (size_t)fl) == 0);
        dec = -99;
        if (ok) { for (i = 0; i < got; i++) dec = hw_dc_proto_feed(rx[i]); }
        ok = ok && (dec == HW_DCPP_FEED_FRAME) &&
             (hw_dc_proto_resp_value() == (int)HW_DCPP_PONG);
        ESP_LOGI(TAG, "[C2] DCPP PING over UART  frame=%dB got=%d feed=%d resp=0x%02X %s",
                 fl, got, dec, (unsigned)hw_dc_proto_resp_value(), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [C3] 会话一条龙过 UART */
        static const uint8_t seq[DCPP_CMD_NUM] = {
            DCPP_CMD_PING, DCPP_CMD_COUNT, DCPP_CMD_OPEN, DCPP_CMD_SET,
            DCPP_CMD_GET, DCPP_CMD_RANGE, DCPP_CMD_DATA, DCPP_CMD_BASE,
            DCPP_CMD_CLOSE
        };
        uint32_t nrx = 0, ntx = 0, nerr = 0;
        uint8_t last = 0;
        int ok = 1;

        hw_dc_init(NULL);
        hw_dc_proto_reset();
        for (k = 0; k < (int)DCPP_CMD_NUM; k++) {
            fl = dc_build_cmd(seq[k], tx, (int)sizeof(tx));
            if (fl <= 0) { ok = 0; break; }
            got = dc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
            if (got != fl || memcmp(rx, tx, (size_t)fl) != 0) { ok = 0; break; }
            dec = 0;
            for (i = 0; i < got; i++) dec = hw_dc_proto_feed(rx[i]);
            if (dec != HW_DCPP_FEED_FRAME) { ok = 0; break; }
        }
        hw_dc_proto_stats(&nrx, &ntx, &nerr, &last);
        ok = ok && (nrx == (uint32_t)DCPP_CMD_NUM) && (nerr == 0) &&
             (last == (uint8_t)DCPP_CMD_CLOSE);
        ESP_LOGI(TAG, "[C3] session over UART  frames=%u err=%u last=%u %s",
                 (unsigned)nrx, (unsigned)nerr, (unsigned)last, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [C4] 损坏帧过 UART → 必须被校验拒绝 */
        hw_dc_init(NULL);
        hw_dc_proto_reset();
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, tx, (int)sizeof(tx));
        tx[7] ^= 0xFF;                              /* 破坏 CKSUM 字节 */
        got = dc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        int ok = (got == fl) && (memcmp(rx, tx, (size_t)fl) == 0);   /* 坏字节原样传过去 */
        /* ⚠️ 必须取**首个**负返回码并立即停止：
         * 状态机在 CKSUM 字节即报错并复位回 IDLE，若继续把后面的 END 字节
         * 喂进去，0xED 会被当成新帧首字节 → 又返回 ERR_FRAME(-1)，把真正的
         * ERR_CKSUM(-2) 覆盖掉（这正是第一次实测 FAIL 的原因：协议对、用例错）。*/
        dec = 0;
        if (ok) {
            for (i = 0; i < got; i++) {
                int r = hw_dc_proto_feed(rx[i]);
                if (r < 0) { dec = r; break; }
            }
        }
        ok = ok && (dec == HW_DCPP_ERR_CKSUM);
        ESP_LOGI(TAG, "[C4] corrupted frame over UART  got=%d first-err=%d (expect %d) %s",
                 got, dec, HW_DCPP_ERR_CKSUM, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [C5] 垃圾后重同步 */
        static const uint8_t garb[3] = { 0x11, 0x22, 0x33 };
        hw_dc_init(NULL);
        hw_dc_proto_reset();
        got = dc_hw_uart_roundtrip(garb, 3, rx, (int)sizeof(rx), 100);
        for (i = 0; i < got; i++) (void)hw_dc_proto_feed(rx[i]);   /* 垃圾应被丢弃 */
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, tx, (int)sizeof(tx));
        got = dc_hw_uart_roundtrip(tx, fl, rx, (int)sizeof(rx), 100);
        int ok = (got == fl) && (memcmp(rx, tx, (size_t)fl) == 0);
        dec = 0;
        if (ok) { for (i = 0; i < got; i++) dec = hw_dc_proto_feed(rx[i]); }
        ok = ok && (dec == HW_DCPP_FEED_FRAME) &&
             (hw_dc_proto_resp_value() == (int)HW_DCPP_PONG);
        ESP_LOGI(TAG, "[C5] resync after garbage  feed=%d resp=0x%02X %s",
                 dec, (unsigned)hw_dc_proto_resp_value(), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    dc_hw_uart_close();

    ESP_LOGI(TAG, "==== hw_dc Phase C (DCPP 帧层真机传输) %s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘");
    return fail;
}

/* ============================================================
 * 入口
 * ============================================================ */
static void dc_diag_task(void *arg)
{
    int fa, fb, fc;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_dc 真机验证开始 (ESP32-S3) ====");
    dc_hw_dump();

#if DC_PIN_SCAN
    /* 板级体检（换板排查用：-DDC_PIN_SCAN=1）。默认关闭，产品构建不含。 */
    ESP_LOGW(TAG, "!! DC_PIN_SCAN=1 体检模式（非产品构建）!!");
    if (dc_hw_hw_init() == 0) {
        dc_hw_drive_readback();
        (void)dc_hw_scan_adc();
    }
#endif

    fa = phase_a();
    fb = phase_b();
    fc = phase_c();

    ESP_LOGI(TAG, "==== hw_dc 真机验证 %s (A=%d B=%d C=%d, total=%d) ====",
             (fa + fb + fc == 0) ? "PASS ✔" : "FAIL ✘", fa, fb, fc, fa + fb + fc);

    vTaskDelete(NULL);
}

void dc_diag_start(void)
{
    xTaskCreate(dc_diag_task, "hw_dc", 8192, NULL, 5, NULL);
}
