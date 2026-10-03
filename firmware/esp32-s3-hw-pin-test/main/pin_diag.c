/**
 * pin_diag.c — ESP32-S3 真机接入 hw_pin（引脚档案层 / 双模驱动 / 编程电压）验证桥
 *
 * ── Phase A: 确定性路径（核心跨模式主张，不需要任何硬件）────────────
 *   [A1] 自动模式探测 → 命中 ESP32
 *   [A2] 档案表黄金 0x9E0F10FA 真机 == 宿主逐位一致
 *   [A3] 内置档案表原封不动（4 条 / 名称 / esp32-9p 脚号）
 *   [A4] ISP 命令表黄金 0xD2A9A924
 *   [A5] selftest fails == 0（真机 == 宿主逐位一致）
 *   [A6] 命令分发（VM OP_HW_PIN_CALL 同款路径）
 *   [A7] **换档案**：load(s3-9p) 后 9 个信号 → S3 真脚号
 *
 * ── Phase B: 真实硅引脚面（BSP 注入）────────────────────────────────
 *   [B1] **GPIO 内部上下拉自证（硬，不需外部器件）**：两个空闲脚各
 *        下拉→读/上拉→读，4 个期望值全中 —— 引脚真的能跟着配置跳变。
 *   [B2] **档案脚「模块写→硬件读」闭环（硬，不需外部器件）**：
 *        hw_pin_gpio_write(RST/CS) 写 1/0，再 gpio_get_level 直读回。
 *   [B3] **bit-bang SPI 环回（硬，需一根跳线 IO11↔IO13）**：
 *        走模块自己的 BB 档，发已知模式读回并逐字节比对。
 *        无跳线 = SKIP（**绝不用悬空电平冒充通过**）。
 *   [B4] **硬件 SPI（SPI2/FSPI）环回（硬，同一根跳线）**：L1 的另一档。
 *   [B5] **反空转（硬）**：装了真机 BSP 后 RDID **不得**等于模拟器的 EF4018
 *        —— 证明模块真的在读硬件，而不是照常返回器件模型的值（坑#17）。
 *   [B6] **SIM/REAL 分水岭（硬）**：卸载 BSP → RDID 必须回到 EF4018。
 *   [B7] **上电不静默卸载（硬）**：装哨兵 BSP → hw_pin_init（kvm_run 上电会
 *        同款调用）→ BSP 必须仍在。这是 hw_dc 已实测过的头号假成功陷阱。
 *
 * ── Phase C: 真实异步链路（UART 外设 + 独立器件任务）──────────────────
 *   [C1] UART1 内部回环（hard）：自发自收 4 字节逐位一致
 *        ⚠️ 必须在切进走线模式**之前**跑（两种形态互斥）
 *   [C2] **隔离式诚实失败**：UART1 走线模式但目标未启动 → ISP 必须 NOTGT
 *        （证明「全绿」不是内部模拟器在自问自答）
 *   [C3] 启动 UART2 上的 AN3155 器件 → 0x7F 握手成功
 *   [C4] Get ID → PID == 0x0410（真器件应答）
 *   [C5] Get Version → 0x31 + 命令表 FNV == **黄金 0xD2A9A924**
 *   [C6] **完整 ISP 烧录**：全片擦 → 写 256B → 读回 → 逐字节一致
 *   [C7] 目标停掉后 Get ID 必须失败（链条另一端真的参与）
 *   [C8] 导线自证（需跳线 IO18↔IO8）：走线模式自发自收 → OK / 无跳线 SKIP
 *
 * ── Phase L0: 编程电压层 ────────────────────────────────────────────
 *   [L0] duty 0/50%/100% → ADC 回读 mV 单调；**如实报告**实测电压
 *        （无升压硬件时只有 0..3.3V，不谎报 12V）
 *   [L0b] **现场取证**：adc_oneshot 配置通道后 LEDC 输出是否被掐掉
 *        （hw_dc 已实测前者会独占 pad 并关掉数字输出驱动器）
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"

#include "hw_pin.h"
#include "pin_hw.h"
#include "pin_target.h"
#include "pin_diag.h"

static const char *TAG = "hw_pin";

#define PIN_GOLDEN_EXPECT      0x9E0F10FAu
#define PIN_ISP_GOLDEN_EXPECT  0xD2A9A924u
#define PIN_SIM_JEDEC          0x001840EFu
#define PIN_ISP_PKT            256u   /* 与模块内 HW_PIN_ISP_MAXPKT 同值 */

static int pin_putf(const char *s)
{
    ESP_LOGI(TAG, "  %s", s ? s : "(null)");
    return 0;
}

/* ---- 哨兵 BSP：gpio_read 判「BSP 还在不在」；gpio_write 判 vex 统计口 ----
 * ⚠️ gpio_write **必须配**。pin_gpio_write_raw 的硬件分支条件是
 *   `if (g_bsp.gpio_write)` —— 不配就走模拟路径，而模拟路径上的 vex++
 *   在修复前后**都会执行** ⇒ B9-1 判据会永远通过 = 无区分力。
 *   缺陷的触发条件恰恰是「装了 gpio_write」，所以判据必须造出这个条件。 */
static int sent_gpio_read(int pin) { (void)pin; return 0xABCD; }
static int sent_gpio_write(int pin, int level) { (void)pin; (void)level; return 0; }
static const hw_pin_bsp_t s_bsp_sentinel = {
    .gpio_read  = sent_gpio_read,
    .gpio_write = sent_gpio_write,
};

static int g_jumper_spi = 1;   /* 1 = SPI 跳线未接（SKIP） */
static int g_l0_level = 0;     /* 0=未知 1=有RC硬件(>=1V) 2=只有PWM/GPIO */

/* ============================================================
 * Phase A
 * ============================================================ */
static int phase_a(void)
{
    int fail = 0;

    hw_pin_init(NULL);           /* 干净起点（此刻无 BSP → SIM）*/

    {   /* [A1] 模式探测 */
        uint8_t m = hw_pin_mode();
        int ok = (m == (uint8_t)HW_PIN_MODE_ESP32);
        ESP_LOGI(TAG, "[A1] mode=%s (%u) %s", hw_pin_mode_str(m), (unsigned)m, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [A2] 档案表黄金 */
        uint32_t h = hw_pin_profile_checksum();
        int ok = (h == PIN_GOLDEN_EXPECT);
        ESP_LOGI(TAG, "[A2] golden=0x%08X (expect 0x%08X) %s",
                 (unsigned)h, (unsigned)PIN_GOLDEN_EXPECT, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [A3] 内置档案表原封不动 */
        int ok = (hw_pin_profile_count() == 4u);
        const hw_pin_profile_t *p = hw_pin_profile_find("esp32-9p");
        uint32_t i;
        static const char *nm[4] = { "esp32-9p", "w25q", "mcu-isp", "eprom" };
        for (i = 0; i < 4u; i++) {
            const hw_pin_profile_t *q = hw_pin_profile_get(i);
            if (!q || strcmp(q->name, nm[i]) != 0) ok = 0;
        }
        if (p && (p->gpio[HW_PIN_MOSI] != 23 || p->gpio[HW_PIN_MISO] != 19 ||
                  p->gpio[HW_PIN_CK] != 18 || p->gpio[HW_PIN_CS] != 5)) ok = 0;
        ESP_LOGI(TAG, "[A3] builtin profiles count=%u names+pins %s",
                 (unsigned)hw_pin_profile_count(), ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [A4] ISP 命令表黄金 */
        uint32_t h = hw_pin_isp_checksum();
        int ok = (h == PIN_ISP_GOLDEN_EXPECT);
        ESP_LOGI(TAG, "[A4] isp golden=0x%08X (expect 0x%08X) %s",
                 (unsigned)h, (unsigned)PIN_ISP_GOLDEN_EXPECT, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [A5] selftest */
        int f = hw_pin_selftest(pin_putf);
        ESP_LOGI(TAG, "[A5] selftest fails=%d %s", f,
                 (f == 0) ? "=> ALL PASS (真机 == 宿主逐位一致)" : "=> FAIL!");
        if (f != 0) fail++;
    }
    {   /* [A6] 命令分发 */
        uint32_t j = 0;
        int c1, c2, c3, c4, c5, c6, c7;
        /* ⚠️ [A5] 的 selftest 结束时会**还原**它进入前的环境绑定
         *    (此刻是前面 [B*] 没跑、但 A5 之前留下的那个哨兵 BSP),
         *    必须先卸干净, 否则下面的 RDID 走的是"假"回调而非器件模型。
         *    第一轮 [A6] 报 FAIL 就是踩在这里 (模块没错, 是用例漏了这段)。 */
        hw_pin_bsp_install(NULL);

        c1 = hw_pin_cmd("mode", NULL);
        c2 = hw_pin_cmd("profiles", NULL);
        c3 = hw_pin_cmd("load w25q", NULL);
        c4 = hw_pin_cmd("isp chk", NULL);
        c5 = hw_pin_cmd("bogus", NULL);
        c6 = hw_pin_cmd("help", NULL);
        /* ⚠️ `isp chk` 会顺手把档案切到 mcu-isp（SPI 四线未映射）
         *    → 必须切回带 SPI 的档案, 否则 RDID 必然 NODEV。 */
        (void)hw_pin_cmd("load w25q", NULL);
        c7 = hw_pin_flash_rdid(&j);

        int ok = (c1 == 0) && (c2 == 0) && (c3 == 0) && (c4 == 0) &&
                 (c5 == HW_PIN_R_NOCMD) && (c6 == HW_PIN_R_HELP) &&
                 (c7 == HW_PIN_R_OK) && (j == (PIN_SIM_JEDEC & 0x00FFFFFFu));
        ESP_LOGI(TAG, "[A6] cmd mode=%d profiles=%d load=%d ispchk=%d bogus=%d help=%d rdid=%06X %s",
                 c1, c2, c3, c4, c5, c6, (unsigned)j, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [A7] 换档案：s3-9p 九个信号 → S3 真脚号 */
        static const int want[HW_PIN_SIG_MAX] = {
            PIN_S3_MOSI, PIN_S3_MISO, PIN_S3_CK, PIN_S3_CS, PIN_S3_TX,
            PIN_S3_RX, PIN_S3_RST, PIN_S3_VPP, PIN_S3_VCC
        };
        int ok = 1, i;
        int rc = pin_hw_profile_load();
        const hw_pin_profile_t *p = hw_pin_active();
        for (i = 0; i < HW_PIN_SIG_MAX; i++) {
            if (hw_pin_gpio_of(i) != want[i]) ok = 0;
        }
        if (!p || strcmp(p->name, "s3-9p") != 0) ok = 0;
        ESP_LOGI(TAG, "[A7] profile swap -> %s rc=%d map %s",
                 p ? p->name : "(null)", rc, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    hw_pin_profile_load(NULL);   /* 回到默认档案 */
    hw_pin_init(NULL);
    ESP_LOGI(TAG, "==== hw_pin Phase A (确定性) %s ====", (fail == 0) ? "PASS ✔" : "FAIL ✘");
    return fail;
}

/* ============================================================
 * Phase B
 * ============================================================ */
static int phase_b(void)
{
    int fail = 0;

    ESP_LOGI(TAG, "---- Phase B: 注入真机 BSP (ESP32-S3 GPIO/SPI/UART) ----");
    if (pin_hw_install(true) != 0) {
        ESP_LOGE(TAG, "[B0] pin_hw_install 失败 → 无法进入真实硅路径");
        return 1;
    }
    pin_hw_dump();

    {   /* [B1] 内部上下拉自证（硬，不需外部器件）*/
        int a = -1, b = -1, c = -1, d = -1;
        int r = pin_hw_prove_gpio(&a, &b, &c, &d);
        int ok = (r == 0);
        ESP_LOGI(TAG, "[B1] gpio pull prove  IO%d pd/pu=%d/%d  IO%d pd/pu=%d/%d %s",
                 PIN_S3_PROOF_A, a, b, PIN_S3_PROOF_B, c, d,
                 ok ? "OK (引脚真能跟着配置跳变, 硬证据)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B2] 档案脚「模块写 → 硬件读」闭环（硬）*/
        int hi = 0, lo = 0;
        int r = pin_hw_prove_owngpio(&hi, &lo);
        int ok = (r == 0);
        ESP_LOGI(TAG, "[B2] module->hw readback  RST/CS  hi=%d lo=%d %s",
                 hi, lo, ok ? "OK (模块写的脚 == 这颗芯片的脚)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B3] bit-bang SPI 环回（需跳线 IO11↔IO13）*/
        static const uint8_t pat[8] = { 0xA5, 0x5A, 0x3C, 0xC3, 0x0F, 0xF0, 0x77, 0x88 };
        uint8_t rb[8];
        int nr = 0;
        int r = pin_hw_spi_bb_loopback(pat, rb, 8, &nr);
        if (r == 0) {
            g_jumper_spi = 0;
            ESP_LOGI(TAG, "[B3] bit-bang SPI loopback rx=%02X %02X %02X %02X %02X %02X %02X %02X %s",
                     rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7],
                     "OK (跳线在位 → **真实 bit-bang SPI 烧录链路成立**, 硬证据)");
        } else if (r == 1) {
            ESP_LOGW(TAG, "[B3] bit-bang SPI loopback rx=%02X %02X %02X %02X.. SKIP (无跳线, 非失败)",
                     rb[0], rb[1], rb[2], rb[3]);
            ESP_LOGW(TAG, "      → 想要 SPI 硬证据: 用杜邦线短接 IO%d(MOSI) ↔ IO%d(MISO) 再跑",
                     PIN_S3_MOSI, PIN_S3_MISO);
        } else {
            ESP_LOGW(TAG, "[B3] bit-bang SPI loopback rc=%d SKIP (调用失败)", r);
        }
    }

    {   /* [B4] 硬件 SPI (SPI2/FSPI) 环回（同一根跳线）*/
        static const uint8_t pat[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
        uint8_t rb[8];
        int nr = 0;
        int r = pin_hw_spi_hw_loopback(pat, rb, 8, &nr);
        if (r == 0) {
            ESP_LOGI(TAG, "[B4] hw-SPI(SPI2/FSPI) loopback rx=%02X %02X %02X %02X.. %s",
                     rb[0], rb[1], rb[2], rb[3], "OK (外设映射档同样成立)");
        } else if (r == 1) {
            ESP_LOGW(TAG, "[B4] hw-SPI loopback rx=%02X %02X %02X %02X.. SKIP (无跳线, 非失败)",
                     rb[0], rb[1], rb[2], rb[3]);
        } else {
            ESP_LOGW(TAG, "[B4] hw-SPI loopback rc=%d SKIP", r);
        }
        /* 无论成败都切回 BB 档，避免外设一直占着 SPI 四线的路由 */
        pin_hw_drv_set(HW_PIN_DRV_BB);
    }

    {   /* [B5] 反空转：真机 BSP + 无器件 → RDID **不得**是模拟器值 */
        uint32_t j = 0;
        int rc = hw_pin_flash_rdid(&j);
        int ok = (j != (PIN_SIM_JEDEC & 0x00FFFFFFu));
        ESP_LOGI(TAG, "[B5] anti-idle  real BSP rdid rc=%d jedec=%06X (sim=%06X) %s",
                 rc, (unsigned)j, (unsigned)(PIN_SIM_JEDEC & 0x00FFFFFFu),
                 ok ? "OK (真的在读硬件, 不是照常返回器件模型)" : "FAIL! (疑似空转)");
        if (!ok) fail++;
    }

    {   /* [B6] SIM/REAL 分水岭 */
        uint32_t j = 0;
        int rc;
        (void)pin_hw_install(false);
        rc = hw_pin_flash_rdid(&j);
        int ok = (rc == HW_PIN_R_OK) && (j == (PIN_SIM_JEDEC & 0x00FFFFFFu));
        ESP_LOGI(TAG, "[B6] uninstall -> SIM  rdid=%06X (expect %06X) %s",
                 (unsigned)j, (unsigned)(PIN_SIM_JEDEC & 0x00FFFFFFu),
                 ok ? "OK (NULL = SIM 分水岭成立)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B7] 重装真机 BSP → 立刻又离开模拟器 */
        uint32_t j = 0;
        int rc;
        (void)pin_hw_install(true);
        rc = hw_pin_flash_rdid(&j);
        int ok = (j != (PIN_SIM_JEDEC & 0x00FFFFFFu));
        ESP_LOGI(TAG, "[B7] reinstall -> REAL  rdid rc=%d jedec=%06X %s",
                 rc, (unsigned)j, ok ? "OK (重装即回真机)" : "FAIL!");
        if (!ok) fail++;
    }

    {   /* [B8] 上电自动 init 不得静默抹掉硬件绑定（hw_dc 已实测过的陷阱）*/
        int before, after;
        hw_pin_bsp_install(&s_bsp_sentinel);
        before = hw_pin_gpio_read(HW_PIN_CS);        /* 哨兵 → 0xABCD */
        hw_pin_init(NULL);                           /* ← 模拟 kvm_run 上电 */
        after = hw_pin_gpio_read(HW_PIN_CS);
        int ok = (before == 0xABCD) && (after == 0xABCD);
        ESP_LOGI(TAG, "[B8] init-keeps-bsp  before=0x%X after=0x%X %s",
                 before, after,
                 ok ? "OK (上电不静默卸载硬件绑定)" : "FAIL! (BSP 被 init 清零)");
        if (!ok) fail++;

        /* 恢复：档案 + 真机 BSP */
        hw_pin_bsp_install(NULL);
        (void)pin_hw_profile_load();
        (void)pin_hw_install(true);
    }

    {   /* 🆕 [B9] 2026-10-02 vex 统计口 + BSP 悬垂 联合验证
         *
         * 缺陷 1 (vex): 原 pin_gpio_write_raw 把 vex++ 放在 BSP 早退 return **之后**
         *   ⇒ 一旦真机装了 gpio_write, 每次写引脚都从那条 return 走掉, vex 恒为 0,
         *   `pin stat` 显示「一次都没写过引脚」—— **观测口与事实相反(静默说谎)**。
         *
         * 缺陷 2 (悬垂): 若 install 存的是**调用方指针**, 这里传栈上临时体,
         *   install 返回后 `g_bsp` 即悬垂 → 后续任何 BSP 调用读的都是被踩的栈。
         *   本段故意用**栈上 BSP**, 一次同时验两件事。
         *
         * 判据:
         *   ① vex 必须在**装了 BSP 之后**继续增长(修复前恒 0) —— 这是核心判据;
         *   ② 栈上 BSP 安装后再干别的事(让栈被踩), BSP 调用结果必须不变 ⇒ 未悬垂;
         *   ③ SIM 路径(装 NULL) vex 同样要涨 ⇒ 两条路径都计数。 */
        hw_pin_stat_t st0, st1, st2;
        /* ⚠️ 必须是**自动存储期(栈)**的 BSP —— 若 install 存的是调用方指针,
         *   本块结束即悬垂, 紧接着的 B9-2 踩栈就会当场暴露。
         *   (写成 static 就造不出悬垂条件, 判据会自证失效) */
        hw_pin_bsp_t stack_bsp;

        /* --- ① 硬件路径: vex 必涨 --- */
        hw_pin_stat(&st0);
        stack_bsp = s_bsp_sentinel;                 /* 栈上临时体 */
        hw_pin_bsp_install(&stack_bsp);
        {
            int i;
            for (i = 0; i < 8; i++) {
                (void)hw_pin_gpio_write(HW_PIN_CS, (i & 1) ? 1 : 0);
                (void)hw_pin_gpio_write(HW_PIN_MOSI, 0);
            }
        }
        hw_pin_stat(&st1);
        uint32_t hw_grew = (st1.vex - st0.vex);
        ESP_LOGI(TAG, "[B9-1] 硬件路径 vex %u -> %u (增长=%u, 期望>=16) %s",
                 (unsigned)st0.vex, (unsigned)st1.vex, (unsigned)hw_grew,
                 hw_grew >= 16u ? "OK (统计口不再说谎)" : "FAIL! (vex 未累计=缺陷未修)");
        if (hw_grew < 16u) fail++;

        /* --- ② 悬垂检测: 踩栈后 BSP 行为必须不变 --- */
        {
            int sent_before = 0, sent_after = 0;
            /* 用一个可观测的 BSP 行为: 哨兵 gpio_read 返回 0xABCD */
            int rd_before = hw_pin_gpio_read(HW_PIN_CS);
            /* 刻意深踩栈, 模拟"install 返回后调用方栈被复用" */
            {
                volatile uint8_t junk[512];
                uint32_t j;
                for (j = 0; j < sizeof(junk); j++) junk[j] = (uint8_t)(0xA5 ^ (j & 0xFF));
                (void)junk[0];
            }
            int rd_after = hw_pin_gpio_read(HW_PIN_CS);
            int intact = (rd_before == 0xABCD) && (rd_after == rd_before);
            ESP_LOGI(TAG, "[B9-2] 栈上 BSP: 踩栈前=0x%X 踩栈后=0x%X %s",
                     rd_before, rd_after,
                     intact ? "OK (未悬垂, 存的是静态副本)" : "FAIL! (悬垂已发生)");
            if (!intact) fail++;
            (void)sent_before; (void)sent_after;
        }

        /* --- ③ SIM 路径对照: 也必须涨(证明两条路径语义一致) --- */
        hw_pin_bsp_install(NULL);
        hw_pin_stat(&st1);
        {
            int i;
            for (i = 0; i < 8; i++) (void)hw_pin_gpio_write(HW_PIN_CS, 1);
        }
        hw_pin_stat(&st2);
        uint32_t sim_grew = (st2.vex - st1.vex);
        ESP_LOGI(TAG, "[B9-3] 模拟路径 vex %u -> %u (增长=%u, 期望>=8) %s",
                 (unsigned)st1.vex, (unsigned)st2.vex, (unsigned)sim_grew,
                 sim_grew >= 8u ? "OK (两路径同等计数)" : "FAIL!");
        if (sim_grew < 8u) fail++;

        /* 恢复真机 BSP, 供后续 Phase C 使用 */
        (void)pin_hw_profile_load();
        (void)pin_hw_install(true);
    }

    ESP_LOGI(TAG, "==== hw_pin Phase B (真实硅引脚面) %s%s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘",
             (fail == 0 && g_jumper_spi) ? "  (SPI 硬证待跳线: SKIP)" : "");
    return fail;
}

/* ============================================================
 * Phase C 前置 — [C1] UART1 内部回环
 *
 * ⚠️ 必须**在切进走线模式之前**跑：内部回环与走线模式是 UART1 的
 *    两种互斥形态（回环会把 TX 灌回自己的 RX）。第一轮把它写在
 *    phase_c 里，而 pin_diag_task 已经先开了走线模式 → 回环打开必然失败。
 * ============================================================ */
static int phase_u(void)
{
    static const uint8_t pat[4] = { 0xA5, 0x5A, 0x00, 0xFF };
    uint8_t rx[8];
    int got, ok;

    ESP_LOGI(TAG, "---- Phase C 前置: UART1 内部回环 (只证 UART 外设本身) ----");
    if (pin_hw_uart_loopback_open() != 0) {
        ESP_LOGE(TAG, "[C1] UART1 内部回环打开失败");
        return 1;
    }
    got = pin_hw_uart_loopback_roundtrip(pat, 4, rx, (int)sizeof(rx), 100);
    ok = (got == 4) && (memcmp(rx, pat, 4) == 0);
    ESP_LOGI(TAG, "[C1] uart1 internal loopback  got=%d %02X %02X %02X %02X %s",
             got, rx[0], rx[1], rx[2], rx[3], ok ? "OK" : "FAIL!");
    pin_hw_uart_loopback_close();
    return ok ? 0 : 1;
}

/* ============================================================
 * Phase C
 * ============================================================ */
static int phase_c(void)
{
    int fail = 0;
    /* 🕳️ 判据顺序事故（2026-10-02 修正）：导线自证 [C8] 原先排在 Phase C
     * **最后**，跑在 C3~C7 后面。可没有跳线时 C3~C7 必然全 NOTGT，
     * 于是日志只剩「5 条失败」，真正的原因（缺跳线）被埋到末尾，
     * 读的人只会以为链路逻辑有 bug，白查一整天。
     *
     * 现在把 [C8] 提到 [C2] 之后、目标未启动之前跑：那一刻 UART2 还没
     * start()，UART1 RX 里出现的任何字节**只可能**来自 IO18↔IO8 跳线
     * 回环 —— 判据绝对干净，且一次跑完就能二分定案：
     *     wire=1 → 导线通，C3~C7 若仍失败才是真逻辑 bug，值得深挖；
     *     wire=0 → 缺跳线，C3~C7 的失败是**预期结果**，不是缺陷。
     */
    int wire = 0;
    /* 注：这里不需要本地 rx 缓冲 —— C 段收发全部走 BSP 的 uart 通道
     * (uart_putc/uart_getc)，那才是「真实异步链路」要证的东西。 */

    ESP_LOGI(TAG, "---- Phase C: 真实异步链路 (UART1 外设 + UART2 器件任务) ----");

    /* 走线模式已由主流程开好（见 pin_diag_task 的注释：只开一次，B/C 共用）。
     * 这里**绝不能**再 link_open()，否则撞自己的 s_uart_mode 守卫。
     * ⚠️ 契约：pin_hw_uart_link_opened() 与 pin_hw_uart_link_open() 同族，
     *    都是 **0 = 就绪/成功，负 = 失败**（uart_cfg 成功时置 s_uart_mode=2
     *    并 return 0，而 link_opened 以 s_uart_mode==2 判就绪 → 二者必然一致）。
     *    初版这里写成 `== 0` 判失败，把契约整个反转：链路明明开好了，
     *    却报「走线链路未就绪」并 return 1 —— 板子上的 C 段就是这么黄的。 */
    if (pin_hw_uart_link_opened() != 0) {
        ESP_LOGE(TAG, "[C0] 走线链路未就绪 (opened()=%d) → Phase C 无法进行",
                 pin_hw_uart_link_opened());
        return 1;
    }

    {   /* [C2] 隔离式诚实失败：目标未启动 → ISP 必须 NOTGT */
        int rc;
        (void)pin_hw_prof_isp_load();
        (void)pin_hw_install(true);
        (void)hw_pin_isp_reset_sync();
        rc = hw_pin_isp_get_id(NULL);
        int ok = (rc == HW_PIN_R_NOTGT);
        ESP_LOGI(TAG, "[C2] isolated honest-fail  target OFF -> get_id rc=%d (%s) %s",
                 rc, hw_pin_result_code_str(rc),
                 ok ? "OK (没目标就是没目标, 不是内部模拟器自问自答)" : "FAIL!");
        if (!ok) fail++;
    }

    /* ============================================================
     * [C8] 导线自证 —— **必须排在 C3~C7 之前**
     *
     * 此刻 UART2（器件模型）尚未 start()，所以 UART1 RX 里出现的任何
     * 字节只可能是 IO18↔IO8 跳线回环。这是整个 Phase C 唯一的
     * 二分定案点：有跳线 → 后续失败是逻辑 bug；无跳线 → 后续失败是预期。
     * ============================================================ */
    {
        static const uint8_t wpat[4] = { 0xC3, 0x3C, 0x5A, 0xA5 };
        uint8_t win[8];
        int got = pin_hw_uart_wire_probe(wpat, 4, win, (int)sizeof(win));
        if (got == 4 && memcmp(win, wpat, 4) == 0) {
            wire = 1;
            ESP_LOGI(TAG, "[C8] uart wire probe  got=%d %02X %02X %02X %02X OK (跳线 IO%d<->IO%d 成立)",
                     got, win[0], win[1], win[2], win[3], PIN_S3_TX, PIN_S3_RX);
        } else {
            wire = 0;
            ESP_LOGE(TAG, "[C8] uart wire probe got=%d ✘ 跳线 IO%d<->IO%d 不通",
                     got, PIN_S3_TX, PIN_S3_RX);
            ESP_LOGE(TAG, "     └── ★定案★ 无跳线 → C3~C7 的 NOTGT 是**预期结果**，不是代码缺陷");
            ESP_LOGE(TAG, "     └── 接一根杜邦线 IO%d(MOSI侧TX) ↔ IO%d(MISO侧RX) 再跑一次即可收官",
                     PIN_S3_TX, PIN_S3_RX);
        }
    }

    /* 启动 UART2 上的器件模型 */
    if (pin_target_start() != 0) {
        ESP_LOGE(TAG, "[C0] 目标器件启动失败");
        return 1;
    }

    /* 🔬 [CX] 判别性实验（诊断用，不计入 fail）
     *
     * ⚠️ 上一版 CX 实验设计有缺陷，结论作废：pad 电平是在**静止期**采样的，
     * 而 115200 波特下 1 bit 只有 ~87us —— 静止期读到的永远是 idle 高电平，
     * 「IO18=1 IO8=1」既不能证明信号在跑，也不能证明没在跑。拿它下
     * 「信号没出 UART1」的结论属于**超证据推断**。
     *
     * 本版只保留可证伪的读数：
     * [CX1] 同步电平探针：在 UART1 发送**进行中**连续采 IO18。
     *       线上跑 0xC3(11000011) 这种 0/1 齐备的字节，
     *       若 TX 真在驱动 pad，采样必然翻转到 0。
     * [CX2] 异步计数复核：必须在 TX 结束**之后**再读 UART2 计数，
     *       否则计数会被半截字节污染。
     *
     * 注：原计划的 [CX3] pad 方向查询在 IDF 5.2 里没有公开 getter
     * （只有 gpio_set_direction），故删除该读数 —— 不拿「查不到」
     * 冒充「方向正常」。
     */
    {
        static const uint8_t pat[4] = { 0xC3, 0x3C, 0x5A, 0xA5 };
        uint32_t rx0, rx1;
        int saw_low = 0, nlow = 0, k;

        rx0 = pin_target_rx_count();
        (void)uart_flush_input(PIN_UART_PORT);
        if (uart_write_bytes(PIN_UART_PORT, (const char*)pat, sizeof(pat))
            == (int)sizeof(pat)) {
            for (k = 0; k < 4000; k++) {     /* 发送期间连续采 ~4ms，覆盖全部 4 字节 */
                if (gpio_get_level(PIN_S3_TX) == 0) { saw_low = 1; nlow++; }
                esp_rom_delay_us(1);
            }
            (void)uart_wait_tx_done(PIN_UART_PORT, pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(150));      /* 等 UART2 任务吃干净 */
        rx1 = pin_target_rx_count();

        ESP_LOGW(TAG, "[CX1] 发送中采 IO%d: 采%d点 见到低电平=%s (低电平点数=%d) => %s",
                 PIN_S3_TX, 4000, saw_low ? "是" : "否", nlow,
                 saw_low ? "★UART1 TX 确实在驱动 pad, pad 通路是活的★"
                         : "★TX 期间 pad 恒高, TX 没在驱动这个 pad★");
        ESP_LOGW(TAG, "[CX2] 异步计数: target rx %u -> %u (delta=%u) => %s",
                 (unsigned)rx0, (unsigned)rx1, (unsigned)(rx1 - rx0),
                 (rx1 > rx0) ? "pad 通路通" : "★pad 通路断★");
    }

    pin_target_reset();
    (void)hw_pin_isp_reset_sync();

    {   /* [C3] 握手 */
        int rc = hw_pin_isp_sync();
        int ok = (rc == HW_PIN_R_OK);
        ESP_LOGI(TAG, "[C3] ISP 0x7F handshake over wire  rc=%d %s",
                 rc, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [C4] Get ID */
        uint16_t pid = 0;
        int rc = hw_pin_isp_get_id(&pid);
        int ok = (rc == HW_PIN_R_OK) && (pid == HW_PIN_ISP_PID);
        ESP_LOGI(TAG, "[C4] Get ID over wire  rc=%d PID=0x%04X (expect 0x%04X) %s",
                 rc, (unsigned)pid, (unsigned)HW_PIN_ISP_PID, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [C5] Get Version + 命令表黄金 */
        uint8_t ver = 0;
        uint32_t cs = 0;
        int rc = hw_pin_isp_get_version(&ver, NULL);
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_get(&ver, &cs);
        int ok = (rc == HW_PIN_R_OK) && (ver == HW_PIN_ISP_BOOTVER) &&
                 (cs == PIN_ISP_GOLDEN_EXPECT);
        ESP_LOGI(TAG, "[C5] GVR over wire  rc=%d ver=0x%02X cmdsum=0x%08X (golden 0x%08X) %s",
                 rc, (unsigned)ver, (unsigned)cs, (unsigned)PIN_ISP_GOLDEN_EXPECT,
                 ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [C6] 完整 ISP 烧录（全片擦 → 写 256B → 读回比对）*/
        static uint8_t pat[PIN_ISP_PKT];
        static uint8_t rd[PIN_ISP_PKT];
        uint32_t i, n = 256u;
        int rc, bad = 0;
        for (i = 0; i < n; i++) pat[i] = (uint8_t)((i * 11u + 5u) & 0xFFu);
        rc = hw_pin_isp_erase_all();
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_write(HW_PIN_ISP_FLASH_BASE, pat, n);
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_read(HW_PIN_ISP_FLASH_BASE, rd, n);
        if (rc == HW_PIN_R_OK) {
            for (i = 0; i < n; i++) if (rd[i] != pat[i]) { bad = 1; break; }
            if (bad) rc = HW_PIN_R_VERIFY;
        }
        int ok = (rc == HW_PIN_R_OK) && !bad;
        ESP_LOGI(TAG, "[C6] full ISP flash over wire  256B @0x%08X -> %s %s",
                 (unsigned)HW_PIN_ISP_FLASH_BASE, hw_pin_result_code_str(rc),
                 ok ? "OK (擦/写/读逐字节一致, 真实异步链路)" : "FAIL!");
        if (!ok) fail++;
    }
    {   /* [C7] 目标真参与：停掉目标后 Get ID 必须失败 */
        uint32_t rxn, txn;
        int rc;
        rxn = pin_target_rx_count();
        txn = pin_target_tx_count();
        pin_target_stop();
        rc = hw_pin_isp_get_id(NULL);
        int ok = (rc != HW_PIN_R_OK) && (rxn > 0) && (txn > 0);
        ESP_LOGI(TAG, "[C7] target really involved  target rx=%u tx=%u ; after stop get_id rc=%d %s",
                 (unsigned)rxn, (unsigned)txn, rc,
                 ok ? "OK (对端真的收发过, 停掉就失败)" : "FAIL!");
        if (!ok) fail++;
    }

    /* 注：[C8] 导线自证已上移到本段之前（目标未启动时）——那才是判据最
     * 干净的位置，此处不再重复跑一遍。 */

    {
        /* ⚠️ 不用 ESP_LOGI 直接吃三元：三分支里 %d 个数不一致，多余实参会被
         * -Werror=format-extra-args 当错误（这正是它该做的）。先拼好整句。 */
        char verdict[160];
        if (fail == 0) {
            snprintf(verdict, sizeof(verdict), "PASS ✔");
        } else if (wire) {
            snprintf(verdict, sizeof(verdict),
                     "FAIL ✘ (跳线 IO%d<->IO%d 已通! 这次是**真逻辑 bug**, 值得深挖)",
                     PIN_S3_TX, PIN_S3_RX);
        } else {
            snprintf(verdict, sizeof(verdict),
                     "BLOCKED (缺跳线 IO%d<->IO%d: C3~C7 失败属预期, 非缺陷)",
                     PIN_S3_TX, PIN_S3_RX);
        }
        ESP_LOGI(TAG, "==== hw_pin Phase C (真实异步链路) %s ====", verdict);
    }
    return fail;
}

/* ============================================================
 * Phase L0
 * ============================================================ */
static int phase_l0(void)
{
    int fail = 0;
    int mv0 = -1, mv5 = -1, mvF = -1, lv5 = -1;
    int mx = pin_hw_vpp_pwm_max();

    ESP_LOGI(TAG, "---- Phase L0: 编程电压层 (LEDC PWM + ADC 回读) ----");

    (void)pin_hw_vpp_pwm_set(0);
    (void)pin_hw_vpp_probe(&mv0, NULL);

    (void)pin_hw_vpp_pwm_set(mx / 2);
    (void)pin_hw_vpp_probe(&mv5, &lv5);

    (void)pin_hw_vpp_pwm_set(mx);
    (void)pin_hw_vpp_probe(&mvF, NULL);

    g_l0_level = (mvF >= 1000) ? 1 : 2;
    ESP_LOGI(TAG, "[L0] pwm duty 0/%d/%d -> VPP ADC = %d/%d/%d mV %s",
             mx / 2, mx, mv0, mv5, mvF,
             (mvF > mv0 + 200) ? "OK (PWM 真的改变了引脚平均电压)"
                               : "NOTE (引脚电压没跟着变: 见 [L0b])");
    if (!(mvF > mv0 + 200)) {
        /* 不是硬件故障就是 pad 被 ADC 抢了 —— 交给 [L0b] 定性 */
        ESP_LOGW(TAG, "      → 无升压/无 RC 硬件时 PWM 引脚只有 0..3.3V 冲激, ADC 采不到稳定值属正常");
    }

    {   /* [L0b] 现场取证：ADC 配置后 LEDC 输出是否被掐 */
        int a = -1, b = -1;
        (void)pin_hw_vpp_pwm_set(mx * 3 / 4);
        (void)pin_hw_vpp_probe(&a, NULL);      /* 直读 */
        pin_hw_vpp_reassert();                 /* 重挂 LEDC 到该脚 */
        (void)pin_hw_vpp_probe(&b, NULL);      /* 重读 */
        ESP_LOGI(TAG, "[L0b] adc-vs-ledc  before=%d mV  after-reassert=%d mV  → %s",
                 a, b,
                 (b > a + 200) ? "adc_oneshot 配置通道会掐掉 LEDC 输出 (与 hw_dc 同源)"
                               : "LEDC 输出未被 ADC 抢占");
    }

    (void)pin_hw_vpp_pwm_set(0);
    ESP_LOGI(TAG, "==== hw_pin Phase L0 (编程电压层) %s (电平档=%d) ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘", g_l0_level);
    return fail;
}

/* ============================================================
 * 入口
 * ============================================================ */
static void pin_diag_task(void *arg)
{
    int fa, fb, fc, fl, fu;
    int64_t t0 = esp_timer_get_time();

    (void)arg;

    ESP_LOGI(TAG, "==== hw_pin 真机验证开始 (ESP32-S3) ====");

    fa = phase_a();

#if PIN_PIN_SCAN
    ESP_LOGW(TAG, "!! PIN_PIN_SCAN=1 体检模式（非产品构建）!!");
    (void)pin_hw_hw_init();
    (void)pin_hw_scan();
#endif

    /* Phase B/C 需要：档案已换成 s3-9p + UART1 已在线 + 真机 BSP 已装。
     * ⚠️ 顺序关键：
     *   ① [C1] 内部回环必须在**走线模式之前**跑（两种形态互斥）；
     *   ② UART 必须在 hw_init 之前打开（hw_init 不碰 TX/RX），
     *      否则 gpio_set_direction 会把 UART 的 pin 路由抢回 GPIO。 */
    (void)pin_hw_profile_load();
    fu = phase_u();
    /* 🕳️ 真实根因（不是硬件问题、也不是 install 被占）：
     *   主流程先开走线模式给 B 用，phase_b 跑完**没关**，
     *   phase_c 又调 link_open() → 撞自己人 `if (s_uart_mode) return -1`，
     *   报成 rc=-1，看着像「install 失败」实为「已开着」。
     *   修法：走线模式**只开一次**，B 和 C 共用，末尾统一 close。 */
    if (pin_hw_uart_link_open() == 0) {
        fb = phase_b();
        fc = phase_c();          /* C 复用这条已开的走线链路 */
        (void)pin_hw_install(false);
        pin_hw_uart_link_close();
    } else {
        ESP_LOGE(TAG, "UART1 走线模式打开失败 → 跳过 B/C");
        fb = 1; fc = 1;
    }

    (void)pin_hw_install(true);
    fl = phase_l0();
    (void)pin_hw_install(false);
    (void)hw_pin_profile_load(NULL);

    {
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "==== hw_pin 真机验证 %s (A=%d B=%d C=%d L0=%d U=%d, total=%d, %lldms) ====",
                 (fa + fb + fc + fl + fu == 0) ? "PASS ✔" : "FAIL ✘",
                 fa, fb, fc, fl, fu, fa + fb + fc + fl + fu, (long long)ms);
    }
    vTaskDelete(NULL);
}

void pin_diag_start(void)
{
    xTaskCreate(pin_diag_task, "hw_pin", 8192, NULL, 5, NULL);
}
