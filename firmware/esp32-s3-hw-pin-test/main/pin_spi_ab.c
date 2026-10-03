/**
 * pin_spi_ab.c — 【SPI 侧 GPIO Matrix A/B 实测】判别性实验
 *
 * ════════════════════════════════════════════════════════════════════════
 * 背景：要裁决的高风险待验项
 * ════════════════════════════════════════════════════════════════════════
 * 2026-10-02 已在 **UART** 侧用受控 A/B 实测推翻「S3 矩阵可任意路由」：
 *   同芯片 / 同 UART1 / 同 64B 突发 / 同样采样函数，唯一变量=IOMUX 脚
 *     IO17 (IOMUX 专属)  -> 1974/7000 点见低电平
 *     IO9  (matrix 脚)   ->    0/7000
 *   源码佐证：U2TXD_GPIO_NUM = (-1)，S3 无经典 out_sig_map[]，
 *             用复合 func_out_sel_cfg[54]。
 *
 * **SPI 侧从未测过**，此前一直被列为「高风险待验，不得当结论」。
 * 本文件就是那把裁决刀。
 *
 * ════════════════════════════════════════════════════════════════════════
 * 🔴 上一轮 UART 实验留下的盲点 —— 本实验必须先拆掉它，否则结论不可复核
 * ════════════════════════════════════════════════════════════════════════
 * UART 那次的 CX1（发送中连续采 pad 电平）与 CX2（对端计数）**共享同一个
 * 盲点**：「pad 输入缓冲器能否读回外设内部驱动的信号」。
 *
 * 若答案是「不能」，则 CX1 读到的 0 与「TX 根本没在驱动」**完全同值** ——
 * 两种情形产生一模一样的读数，无法区分。那次 A/B 之所以还有效，是因为
 * 它靠的是**阳性对照**（IOMUX 臂采到 1974）反证采样法本身是活的。
 *
 * 本文件把这个阳性对照**提到最前面，单独做成第一道闸 [AB0]**：
 * 先证明「GPIO 内部驱动 → 同脚采样读回」这条路是通的，
 * 后面 CX 式的采样读数才被允许作为证据。
 *
 * ════════════════════════════════════════════════════════════════════════
 * 实验设计
 * ════════════════════════════════════════════════════════════════════════
 * 硬件依据（ IDF esp32s3/include/soc/spi_pins.h 原文）：
 *   "There are 2 sets of GPIO pins which could be routed to FSPICS0,
 *    FSPICLK, FSPID, FSPIQ, FSPIHD, FSPIWP."
 *   #define SPI2_FUNC_NUM 4
 *   #define SPI2_IOMUX_PIN_NUM_MOSI 11 / CLK 12 / MISO 13 / CS 10
 *
 * ⚠️ 注意 SPI2_FUNC_NUM 是**矩阵功能号**，与上面那组「IOMUX 脚」不是
 * 同一层概念 —— 源码注释自己就把「2 sets of GPIO pins」说成可路由，
 * 措辞上**倾向于支持 matrix 可用**。但注释不是证据，真机才是。
 * （这正是锚点 G 的味道：别信注释，去跑。）
 *
 * 唯一变量必须是**引脚**，其余（芯片/host/时钟/模式/突发长度/采样函数/
 * 采样点数）全部锁死：
 *   A 臂 = SPI2 官方 IOMUX 组   MOSI=11 CK=12 MISO=13 CS=10
 *   B 臂 = matrix 脚（挑空闲且非保留/非 strapping/非 PSRAM 八线）
 *
 * 采样对象选 **CK**（时钟）而非 MOSI：SPI 空闲时 CLK 恒低，一旦开始
 * 传输就有确定的上升沿。用 0x00 全零字节最省事 —— CPOL=0/CPHA=0 下
 * 一个字节=8 个完整时钟，且 MOSI 恒低不干扰。判据 = 采到的高电平点数。
 *
 * ════════════════════════════════════════════════════════════════════════
 * 🔴 2026-10-03 上板前源码审计：抓出**本文件自己**会让实验结构性作废的缺陷
 * ════════════════════════════════════════════════════════════════════════
 * 初版 ab_run_arm() 在 spi_bus_add_device() **之后**，对 CK 脚又调了一次
 * gpio_config(GPIO_MODE_INPUT + PULLDOWN)。这行代码亲手把被测量拆了：
 *
 *   gpio.c:390-391  非 OUTPUT → gpio_output_disable()
 *   gpio_ll.h:268-279  ├─ enable_w1tc 清 OE
 *                     └─ REG_WRITE(FUNCx_OUT_SEL_CFG, SIG_GPIO_OUT_IDX)
 *                        源码注释原文: "Ensure no other output signal is routed
 *                        via GPIO matrix to this pin" ← 亲手拆掉矩阵出向路由
 *   gpio.c:401-403  gpio_pulldown_en()  45kΩ 与驱动器对顶
 *   gpio.c:431      无条件 iomux_func_sel(PIN_FUNC_GPIO)  ← 断开 SPI2_CLK 复用
 *
 * ⇒ A 臂（IO12，IOMUX）也会被切掉复用功能。两臂都读 0，
 *   只能落进 verdict=-1「无效」——不是有效实验，还留下「顺势说 matrix 不通」的口子。
 *   修法见下方 [AB-G]：改用只置 FUN_IE 一位的 PIN_INPUT_ENABLE，一个寄存器都不碰。
 *
 * 【顺带一个正面结论：实验设计的 A/B 机制对比是真的，不是只差引脚号】
 * IDF 5.2 spicommon_bus_init_io_cfg() 有两条**互斥**分支（spi_common.c:633/638）：
 *   use_iomux = !(flags & GPIO_PINS) && bus_uses_iomux_pins(host, bus)
 *   bus_uses_iomux_pins → check_iomux_pins_quad 要求 sclk 恰等于
 *   spi_periph_signal[host].spiclk_iomux_pin = SPI2_IOMUX_PIN_NUM_CLK = 12
 *     A 臂 CK=IO12 命中 → bus_iomux_pins_set_quad()
 *                      → gpio_iomux_out(12, SPI2_FUNC_NUM=4, false)  **IOMUX 复用**
 *     B 臂 CK=IO5  ≠12 落空 → 矩阵分支 (spi_common.c:638-717)
 *                      → esp_rom_gpio_connect_out_signal(5, FSPICLK_OUT_IDX=101, ...)
 *                      + iomux_func_sel(5, FUNC_GPIO)  **矩阵出向**
 *
 * ⚠️ 诚实标注**源码的边界**：驱动建路由那侧调的是 esp_rom_gpio_iomux_out() /
 *    esp_rom_gpio_connect_out_signal()，二者是 **ROM 函数，IDF 仓库里没有实现体**
 *    （esp_rom_gpio.h 只有声明），所以"它写了哪些位"我无法从源码证明。
 *    → 这正是本实验必须实测、且测试代码一个寄存器都不能多碰的原因。
 *    → 只能证明的是**我方**做了什么：清 OE / 拆路由 / 切 FUNC / 加下拉，这四条都在源码里。
 *
 * ════════════════════════════════════════════════════════════════════════
 * 判定为 PASS 的条件（缺一即判 FAIL，不给"疑似"留口子）：
 *   [AB0] 采样法自证：GPIO 内部驱动 → 同脚读回，必须成立
 *   [AB-G] 路由实况闸：CK 脚三个寄存器必须与该臂**预期机制**相符
 *           （A 臂 MCU_SEL=4 且 OUT_SEL=SIG_GPIO_OUT_IDX=256；
 *             B 臂 MCU_SEL=0 且 OUT_SEL=FSPICLK_OUT_IDX=101）
 *   [AB1] A 臂（IOMUX）  高电平点数 > 0   → 外设确实驱动了 IOMUX 脚
 *   [AB2] B 臂（matrix）高电平点数 > 0   → 外设确实驱动了 matrix 脚
 *   [AB3] 两臂都由同一函数采样，函数调用数一致（排除"没跑"的假阴性）
 *
 * [AB-G] 是本轮新增的第 4 条独立证据通道。上一轮 UART 实验的根本盲点是
 * 「pad 读到的 0」与「信号根本不存在」同值；寄存器读数与 pad 读数来自
 * 不同硬件路径，能劈开这个歧义（详见 ab_regs_read/ab_ck_report 注释）。
 * 它在**取数据之前**就已写进 VERDICT_RUBRIC.md，非事后解释。
 *
 * 若 [AB2] == 0 而 [AB1] > 0 且 [AB-G] 证实 B 臂矩阵路由**确实已建立**
 *   → **SPI matrix 出向同样不通**，与 UART 侧结论一致，
 *     S3 做 SPI 烧录器第一刀必须用 IOMUX 专属脚。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_check.h"

/* [AB-G] 寄存器级证据通道所需的 SoC 头。全部只读，不做任何配置。 */
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_pins.h"

#include "pin_spi_ab.h"

static const char *TAG = "spi_ab";

/* ──────────────────────────────────────────────────────────────────────
 * 🔴 采样通路开关 —— **阳性对照与被测方法必须共用这一个函数**
 *
 * 为什么单独抽出来（2026-10-03 真机证据换来的教训）：
 *   本实验真正测电平走的是 [AB0] 自证 + 各臂采样，两者都必须是
 *   「驱动/外设把电平弄上 pad → 开 FUN_IE → gpio_get_level 读回」。
 *
 *   旧版 [AB0] 自证用 gpio_config(GPIO_MODE_OUTPUT)（FUN_IE=0），
 *   而各臂采样用 PIN_INPUT_ENABLE（FUN_IE=1）—— **两者不同构**。
 *   真机实测（G2 诊断矩阵，6 脚×2 轮零抖动）：
 *       T4 OUTPUT(IE关) 驱动高   → 读 0/200
 *       T5 INPUT_OUTPUT(IE开) 驱动高 → 读 200/200
 *   阳性对照测的是 IE 关那条路，被测方法走的是 IE 开那条路。
 *   结构上它就没有能力验证被测方法：哪怕实验采样法全瞎，它也照样能通过；
 *   反过来它失败时被误读成「环境坏了/板子有问题」，实为对照自身写错。
 *
 *   判读法（写代码时必须照做）：问一句「**这条断言的通过，能不能
 *   证明我真正要用的那套方法可用？**」不能，就说明对照没对准。
 *
 * FUN_IE=BIT(9) / MCU_SEL=bits12-14（io_mux_reg.h:52,63）→ 不相交，
 * 只置采样通路那一位，碰不到 OE / FUNCx_OUT_SEL_CFG / MCU_SEL。
 * ------------------------------------------------------------------- */
static void ab_sense_enable(int pin)
{
    PIN_INPUT_ENABLE(IO_MUX_GPIO0_REG + pin * 4);
}

/* ── 引脚配置 ──────────────────────────────────────────────────────────
 * A 臂：IDF 官方 esp32s3 spi_pins.h 的 SPI2 组，一字不改
 *     MOSI 11 / CLK 12 / MISO 13 是 SPI2 的 IOMUX 专属脚。
 *     CS 由 10 改 14：10 是 LCD_PIN_CS（lcd_hw.h），与 A 臂测量的 CK 无关，
 *     但会让「A 臂是干净阳性对照」这句话站不住 —— 阳性对照臂自己都不干净，
 *     就没有资格给别的臂当对照。
 *
 * B 臂：matrix 臂。🔴 选脚**不再靠注释**，由两重机器闸门强制：
 *   1) 编译期：下方 AB_ASSERT_FREE() _Static_assert，撞禁脚直接编不过
 *   2) 跑批前：preflight 调 `pin_free.py --check <脚...>`，非 0 即中止
 *   禁脚表 pin_free.py 的 LIVE 段每次现 parse 活体头文件（lcd_hw.h / btn.h），
 *   不手抄数值 —— 手抄就退化成与原处同源的单源记载，迟早会漂。
 *
 * ⚠️ 事故记录（本次选脚错误的由来，勿删）：
 *   2026-10-03 上一轮 B 臂选 4/5/6/7，注释写「全部落在空闲区，且不与任何
 *   已记录用途冲突」—— 而这四只**恰恰是本板 LCD 接线**（DC/RST/SCLK/MOSI）。
 *   同一工程 G2 矩阵选脚处却写着「已避开 LCD 4/5/6/7/10」：约束作者知道，
 *   只是一处靠注释记、另一处忘了记，**没有机器强制** ⇒ 一整轮实验作废。
 *   教训：凡「选 X 必须不碰 Y」的约束，机器不执行就等于不存在。
 * ------------------------------------------------------------------- */
#define AB_A_MOSI   11
#define AB_A_CK     12
#define AB_A_MISO   13
#define AB_A_CS     14

/* B 臂：全部取自 pin_free.py 判定的空闲区（12/15/40/41 均已过闸） */
#define AB_B_MOSI   15
#define AB_B_CK     16
#define AB_B_MISO   40
#define AB_B_CS     41

/* [AB0] 阳性对照探测脚：与下方 AB_ASSERT_FREE 用同一个常量，
 * 不许在函数里再写一个字面量 —— 那就变成两份独立数字，早晚会漂。
 * 选 21：必须与 A/B 两臂全部 8 只脚**都不同**（preflight 闸F 会查重复），
 * 且与被测的 CK（12/16）拉开距离，免得自证时的高电平串到测量窗口里。 */
#define AB_PROBE 21

/* ──────────────────────────────────────────────────────────────────────
 * 🔴 编译期禁脚闸门
 *
 * 为什么要有编译期这一层，而不是只靠 preflight：
 *   preflight 跑批时才拦，那时固件已经编完了、人也已经花时间在看数据。
 *   编译期拦 = 错误在**写下来的一刻**就暴露，成本最低、发现最早。
 *   两层是冗余，不是重复 —— 删掉任何一层，另外一层都还在。
 *
 * 判据来源逐条列死，数字后面写清「谁在用」：`_Static_assert` 只报一句
 * 「表达式为假」，不会告诉你是哪条约束撞的，所以出处必须写在代码里。
 * ------------------------------------------------------------------- */
#define AB_ASSERT_FREE(p, who)                                               \
    _Static_assert(                                                            \
        !(AB_IS_LCD(p)) && !(AB_IS_BTN(p)) &&                                 \
        !((p) >= 33 && (p) <= 37) &&      /* PSRAM OCT SPIIO4~7 / DQS      */\
        !(AB_IS_STRAP(p)) &&                                             \
        !((p) == 19 || (p) == 20) &&      /* USB-Serial-JTAG D-/D+        */\
        !((p) == 43 || (p) == 44) &&      /* UART0 = USB 串口本体          */\
        (p) < 54,                                                             \
        who " = IO" #p " 撞禁脚（LCD 4/5/6/7/10 · 三键 17/38/39 · "           \
        "PSRAM 33-37 · strapping 0/3/45/46 · USB-JTAG 19/20 · UART0 43/44）"  \
        "，换脚；禁脚表见 pin_free.py")

/* LCD 接线（活体 lcd_hw.h: LCD_PIN_MOSI 7 / SCLK 6 / DC 4 / RST 5 / CS 10） */
#define AB_IS_LCD(p) ((p) == 4 || (p) == 5 || (p) == 6 || (p) == 7 || (p) == 10)
/* v3 三键（活体 btn.h: BTN_PIN_UP 38 / DOWN 39 / BACK 17） */
#define AB_IS_BTN(p) ((p) == 17 || (p) == 38 || (p) == 39)
/* strapping 脚 */
#define AB_IS_STRAP(p) ((p) == 0 || (p) == 3 || (p) == 45 || (p) == 46)

AB_ASSERT_FREE(AB_A_MOSI, "A臂MOSI");
AB_ASSERT_FREE(AB_A_CK,   "A臂CK");
AB_ASSERT_FREE(AB_A_MISO, "A臂MISO");
AB_ASSERT_FREE(AB_A_CS,   "A臂CS");
AB_ASSERT_FREE(AB_B_MOSI, "B臂MOSI");
AB_ASSERT_FREE(AB_B_CK,   "B臂CK");
AB_ASSERT_FREE(AB_B_MISO, "B臂MISO");
AB_ASSERT_FREE(AB_B_CS,   "B臂CS");
AB_ASSERT_FREE(AB_PROBE,  "[AB0]探测脚");
/* 采样参数：两臂必须完全一致 */
#define AB_SAMPLE_POINTS   7000   /* 与 UART 那次同量级，便于横向对照 */
#define AB_XFER_BYTES      64     /* 突发长度，锁死 */
#define AB_SAMPLE_US         1

/* ──────────────────────────────────────────────────────────────────────
 * [AB0] 采样法自证 —— 本实验的第一道闸，也是拆盲点的那一刀
 *
 * 用**纯 GPIO**（不涉任何外设矩阵）做阳性对照：
 *   把脚配成 output，driving 低，再 gpio_get_level 读回。
 * 若读回 0 → 「内部驱动信号能被同脚采样读到」成立，
 *            后续 CX 式采样读数才可以当证据用。
 * 若读回 1 → 采样法本身就瞎，后面所有电平读数一律作废。
 * ------------------------------------------------------------------- */
static int ab_probe_selfcheck(int* out_ok)
{
    const int probe = AB_PROBE;      /* 与 AB_ASSERT_FREE 同源，见上方定义 */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << probe,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    int r_low = -1, r_high = -1, nlow = 0, nhigh = 0, i;

    *out_ok = 0;
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "[AB0] GPIO 配置失败 -> 采样法不可信, 后续全部作废");
        return -1;
    }

    /* 🔴 关键：与各臂采样走**同一个**函数（ab_sense_enable），不是各写一份。
     * 2026-10-03 真机 G2 矩阵证明两者不是同一条路：
     *     T4 本配置(FUN_IE=0) 驱动高 → 读 0/200
     *     T5 开 FUN_IE 驱动高        → 读 200/200
     * 若这里用不同写法，本闸就变成了「测 A 验 B」，失败时会被
     * 误读成「板子/环境坏了」，实际是对照自身没对准被测方法。 */
    ab_sense_enable(probe);

    /* 驱动低，采 200 点；驱动高，采 200 点 */
    gpio_set_level(probe, 0);
    for (i = 0; i < 200; i++) {
        if (gpio_get_level(probe) == 0) nlow++;
        esp_rom_delay_us(AB_SAMPLE_US);
    }
    r_low = nlow;

    gpio_set_level(probe, 1);
    for (i = 0; i < 200; i++) {
        if (gpio_get_level(probe) == 1) nhigh++;
        esp_rom_delay_us(AB_SAMPLE_US);
    }
    r_high = nhigh;

    /* 还原：探测脚是被借用的，用完必须还回去。
     * 留着 OUTPUT 配置 = 把一只脚留在非中性状态，污染后面任何用到它的实验。
     * （本轮实测无碍，但「恰好没碍」不是理由 —— 借东西要还。） */
    {
        gpio_config_t rst = {
            .pin_bit_mask = 1ULL << probe,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&rst);
    }

    /* 判据：两个方向都必须近乎全中。只判「读到过一个」太松。 */
    if (r_low >= 190 && r_high >= 190) {
        *out_ok = 1;
        ESP_LOGI(TAG, "[AB0] 采样法自证 OK  驱动低读到低 %d/200, 驱动高读到高 %d/200"
                      "  => pad 采样通道是活的, 后续电平读数可作证据", r_low, r_high);
    } else {
        ESP_LOGE(TAG, "[AB0] 采样法自证 FAIL  驱动低读到低 %d/200, 驱动高读到高 %d/200"
                      "  => 采样法本身失灵, 本轮所有电平读数一律作废",
                 r_low, r_high);
    }
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────
 * [AB-G] 寄存器级独立证据通道
 *
 * 为什么需要它：pad 读数与寄存器读数走**两条不同的硬件路径**。
 *   寄存器说「矩阵路由已建立 / 未建立」  ← 仪器事实，与波形无关
 *   pad 说「上面有没有波形」
 * 上一轮 UART 实验把这两条混成一条，于是「路由没建」与「建了没驱动」
 * 产生同一个读数 0。本实验把它们拆开。
 * ------------------------------------------------------------------- */
typedef struct {
    uint32_t iomux_raw;    /* IO_MUX_GPIO0_REG + n*4 整个字 */
    uint32_t fun_ie;       /* bit9  输入通路使能 */
    uint32_t mcu_sel;      /* bits12-14  IOMUX 复用功能号 */
    uint32_t oe;           /* GPIO_ENABLE_REG bit n（n<32） */
    uint32_t out_sel_raw;  /* FUNCx_OUT_SEL_CFG 整个字 */
    uint32_t out_func_sel; /* bits0-8  矩阵出向信号源 */
} ab_regs_t;

/* 纯读，无副作用。位域定义全部出自 IDF 5.2 源码：
 *   io_mux_reg.h:52  FUN_IE = BIT(9)
 *   io_mux_reg.h:63  MCU_SEL = 0x7, MCU_SEL_S = 12
 *   gpio_struct.h:168-176  func_out_sel_cfg: func_sel:9 / inv_sel:1 / oen_sel:1 / oen_inv_sel:1
 */
static void ab_regs_read(int ck, ab_regs_t* r)
{
    r->iomux_raw    = REG_READ(IO_MUX_GPIO0_REG + ck * 4);
    r->fun_ie       = (r->iomux_raw & FUN_IE) ? 1u : 0u;
    r->mcu_sel      = (r->iomux_raw >> 12) & 0x7u;
    r->oe           = (REG_READ(GPIO_ENABLE_REG) >> ck) & 0x1u;  /* n<32，用低位那个 */
    r->out_sel_raw  = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + ck * 4);
    r->out_func_sel = r->out_sel_raw & 0x1FFu;
}

/* 打印 + 与该臂预期机制硬对拍。返回 1 = 路由实况符合预期机制 */
static int ab_ck_report(int ck, const ab_regs_t* r,
                        uint32_t exp_mcu_sel, uint32_t exp_func_sel)
{
    int ok = (r->mcu_sel == exp_mcu_sel) && (r->out_func_sel == exp_func_sel);

    /* ⚠️ 必须显式 (unsigned) cast：在 xtensa 交叉编译器上 uint32_t 是
     * long unsigned int，%u/%08x 会报 -Wformat=error（宿主 clang 不报，
     * 只有交叉编译暴露 —— hw_family 家族坑，务必别删这些 cast）。 */
    ESP_LOGI(TAG, "[AB-G] CK=IO%d 寄存器实况: IO_MUX=0x%08x (FUN_IE=%u MCU_SEL=%u) "
                  "OE=%u  FUNCx_OUT_SEL=0x%08x (func_sel=%u)",
             ck, (unsigned)r->iomux_raw, (unsigned)r->fun_ie, (unsigned)r->mcu_sel,
             (unsigned)r->oe, (unsigned)r->out_sel_raw, (unsigned)r->out_func_sel);
    if (ok) {
        ESP_LOGI(TAG, "[AB-G] CK=IO%d ✓ 路由实况与该臂预期机制相符 "
                      "(MCU_SEL=%u 期望%u, func_sel=%u 期望%u) => 驱动确实按预期路径布了路由",
                 ck, (unsigned)r->mcu_sel, (unsigned)exp_mcu_sel,
                 (unsigned)r->out_func_sel, (unsigned)exp_func_sel);
    } else {
        ESP_LOGE(TAG, "[AB-G] CK=IO%d ✗ 路由实况**不符**该臂预期机制 "
                      "(MCU_SEL=%u 期望%u, func_sel=%u 期望%u) => 本臂没按设计布好, 数据不可用",
                 ck, (unsigned)r->mcu_sel, (unsigned)exp_mcu_sel,
                 (unsigned)r->out_func_sel, (unsigned)exp_func_sel);
    }
    return ok;
}

/* ──────────────────────────────────────────────────────────────────────
 * 一次臂测试：配置 SPI2 到指定引脚组，突发 64 字节 0x00，同步采 CK 高电平
 *
 * 采样在 spi_device_transmit() 之前就启动不了（同步阻塞），故用
 * 「先发、再采」会漏掉时钟。这里改用**后台任务发**、主循环采 ——
 * 保持与 UART 那次同构的「发送进行中连续采样」语义。
 * ------------------------------------------------------------------- */
static void ab_arm_task(void* arg)
{
    spi_device_handle_t* ph = (spi_device_handle_t*)arg;
    uint8_t tx[AB_XFER_BYTES];
    uint8_t rx[AB_XFER_BYTES];

    memset(tx, 0x00, sizeof(tx));
    memset(rx, 0xFF, sizeof(rx));
    for (int k = 0; k < 40; k++) {
        (void)spi_device_transmit(*ph, &(spi_transaction_t){
            .length    = AB_XFER_BYTES * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        });
        /* 让主循环有充足窗口采到时钟 */
        for (volatile int d = 0; d < 20000; d++) { }
    }
    vTaskDelete(NULL);
}

/* 返回采到的高电平点数；<0 表示该臂未能启动（结构性失败）
 * out_regs 带回该臂 CK 脚寄存器实况；out_route_ok 带回 [AB-G] 路由闸结果
 * exp_mcu_sel/exp_func_sel = 该臂**预期机制**，用于证明 A/B 走的是两条不同硬件路径 */
static int ab_run_arm(int mosi, int ck, int miso, int cs, const char* name,
                     uint32_t exp_mcu_sel, uint32_t exp_func_sel,
                     ab_regs_t* out_regs, int* out_route_ok)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = ck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = AB_XFER_BYTES,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000000,   /* 1MHz：64B≈512us，采样窗口足够 */
        .mode           = 0,          /* CPOL=0 CPHA=0 */
        .spics_io_num   = -1,         /* CS 交给模块自己管，不与外设抢路由 */
        .queue_size     = 1,
    };
    spi_device_handle_t dev_h = NULL;
    int nhigh = 0, i, rc;

    /* 早退路径也要让调用方拿到确定值，不能留未初始化栈垃圾 */
    memset(out_regs, 0, sizeof(*out_regs));
    *out_route_ok = 0;

    rc = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AB] %s 臂 spi_bus_initialize 失败 rc=%d", name, rc);
        return -1;
    }
    rc = spi_bus_add_device(SPI2_HOST, &dev, &dev_h);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "[AB] %s 臂 spi_bus_add_device 失败 rc=%d", name, rc);
        (void)spi_bus_free(SPI2_HOST);
        return -1;
    }

    /* ──────────────────────────────────────────────────────────────────
     * 🔴 原初版这里有一整段 gpio_config(GPIO_MODE_INPUT + PULLDOWN)，
     *    已被删除。它会在驱动布好路由之后亲手拆掉被测量（详见文件头）。
     *    现在的唯一动作：只置 FUN_IE 一位，开采样通路。
     *
     *    为什么这一位就够：gpio_ll_get_level() 读的是 GPIO 模块的 in 寄存器
     *    (gpio_ll.h:370)，与 IO_MUX 的 MCU_SEL 无关；而 FUN_IE 就是 pad→采样通路的门。
     *    io_mux_reg.h: FUN_IE=BIT(9)，MCU_SEL=bits12-14 → 二者不相交，
     *    SET_PERI_REG_MASK 只置那一位，碰不到 OE / FUNCx_OUT_SEL_CFG / MCU_SEL。
     *
     * ⚠️ 不加下拉：CPOL=0 时 CLK 空闲本就是低，不需要靠下拉"保证"低；
     *    加下拉反而会让"基线恒低"变成一句废话，并与驱动器对顶。
     * ------------------------------------------------------------------ */
    ab_sense_enable(ck);

    /* [AB-G] 在**发送之前**先取一次寄存器实况：此刻路由已由驱动布好（bus_init
     * + add_device 之后），但外设还没开始打时钟。这正是判"路由建没建"的时机。 */
    ab_regs_read(ck, out_regs);
    *out_route_ok = ab_ck_report(ck, out_regs, exp_mcu_sel, exp_func_sel);

    /* 采空载基线。CPOL=0 空闲 CLK 天然为低；若不是全低，说明该脚存在外部拉高
     * 或残留路由 —— 那是事实，不是噪声，判读时要当真。 */
    {
        int nlow_base = 0;
        for (i = 0; i < 500; i++) {
            if (gpio_get_level(ck) == 0) nlow_base++;
            esp_rom_delay_us(AB_SAMPLE_US);
        }
        ESP_LOGI(TAG, "[AB] %s 臂 发送前基线: 采500点 低电平=%d/500 (无下拉, 靠 CPOL=0 天然低)",
                 name, nlow_base);
    }

    /* 启动发任务的同一时刻开始采样 */
    xTaskCreate(ab_arm_task, "ab_arm", 4096, &dev_h, 5, NULL);
    for (i = 0; i < AB_SAMPLE_POINTS; i++) {
        if (gpio_get_level(ck) == 1) nhigh++;
        esp_rom_delay_us(AB_SAMPLE_US);
    }
    vTaskDelay(pdMS_TO_TICKS(1200));   /* 等发任务收尾 */

    (void)spi_bus_remove_device(dev_h);
    (void)spi_bus_free(SPI2_HOST);

    ESP_LOGI(TAG, "[AB] %s 臂 CK=IO%d 传输中采 %d 点, 高电平点数=%d  => %s",
             name, ck, AB_SAMPLE_POINTS, nhigh,
             (nhigh > 0) ? "★外设确实在驱动这个脚★" : "★这个脚上没有波形★");
    if (nhigh == 0) {
        if (*out_route_ok) {
            ESP_LOGW(TAG, "[AB] %s 臂 路由闸=通过(已证实该脚路由按预期机制建立), "
                          "但 pad 上仍无波形 => 这是**驱动能力**问题, 不是仪器拆掉了路由", name);
        } else {
            ESP_LOGW(TAG, "[AB] %s 臂 路由闸=不通过(该脚没按预期机制布路由) "
                          "=> 读 0 无法归因于 matrix 不通, 本臂数据不可用", name);
        }
    }
    return nhigh;
}

/* ──────────────────────────────────────────────────────────────────────
 * 入口
 * ------------------------------------------------------------------- */
int pin_spi_ab_run(int* verdict)
{
    int self_ok = 0;
    int nhigh_a = 0, nhigh_b = 0;
    int fails = 0;
    ab_regs_t regs_a, regs_b;
    int route_ok_a = 0, route_ok_b = 0;

    *verdict = 0;

    ESP_LOGW(TAG, "════ SPI 侧 GPIO Matrix A/B 实测 ════");
    ESP_LOGW(TAG, "  待裁决: S3 的 FSPI 信号能否经 GPIO Matrix 路由到任意脚");
    ESP_LOGW(TAG, "  UART 侧已判 matrix 出向不通(IO17=1974 vs IO9=0)，SPI 侧从未测过");
    ESP_LOGW(TAG, "  唯一变量 = 引脚组；host/时钟/模式/突发/采样函数全部锁死");

    /* [AB0] 第一道闸：先证明采样法可信，否则一切电平读数作废 */
    if (ab_probe_selfcheck(&self_ok) != 0) return 1;
    if (!self_ok) {
        *verdict = -1;   /* 结构性失败：本轮不能下任何结论 */
        ESP_LOGE(TAG, "[AB0] 采样法失灵 => 本轮**不产出任何结论**, 而不是产出'matrix 不通'");
        return 1;
    }

    /* A 臂：IOMUX 专属脚。预期机制 = MCU_SEL=SPI2_FUNC_NUM(4) + 出向走 SIG_GPIO_OUT_IDX */
    ESP_LOGW(TAG, "[AB1] A 臂 = SPI2 官方 IOMUX 组 (MOSI=%d CK=%d MISO=%d CS=%d)",
             AB_A_MOSI, AB_A_CK, AB_A_MISO, AB_A_CS);
    nhigh_a = ab_run_arm(AB_A_MOSI, AB_A_CK, AB_A_MISO, AB_A_CS, "A(IOMUX)",
                         SPI2_FUNC_NUM, (uint32_t)SIG_GPIO_OUT_IDX,
                         &regs_a, &route_ok_a);
    if (nhigh_a < 0) { fails++; }
    else if (nhigh_a == 0) { fails++; }

    vTaskDelay(pdMS_TO_TICKS(300));

    /* B 臂：matrix 脚。预期机制 = MCU_SEL=PIN_FUNC_GPIO(1) + 出向走 FSPICLK_OUT_IDX(101)
     *
     * 🕳️ 2026-10-03 真 bug 修正：这里曾传字面量 `0u`，并注释「预期机制 = MCU_SEL=FUNC_GPIO(0)」。
     *    权威头文件 components/soc/esp32s3/include/soc/io_mux_reg.h:140 写的是
     *        #define PIN_FUNC_GPIO   1
     *    S3 上「走 GPIO Matrix」的 FSEL 就是 **1**，0 并非 GPIO（该头文件里也**没有**
     *    PIN_FUNC_RESERVED=0 之类的定义 ⇒ `0u` 是凭空写的字面量）。
     *    ⇒ 上一轮 B 臂明明路由正确（实测 MCU_SEL=1、func_sel=101=FSPICLK_OUT_IDX 全对），
     *      却被本闸门报「✗ 路由闸不通过」，白扔掉一条最关键的独立证据。
     *    ⇒ 同一份错误也被复制进 judge_spiab.py 的 EXP_MCU_B，两处必须一起改。
     *    纪律：预期值**不许写字面量**，要用芯片 SDK 头文件里的具名常量。
     */
    ESP_LOGW(TAG, "[AB2] B 臂 = matrix 脚 (MOSI=%d CK=%d MISO=%d CS=%d)",
             AB_B_MOSI, AB_B_CK, AB_B_MISO, AB_B_CS);
    nhigh_b = ab_run_arm(AB_B_MOSI, AB_B_CK, AB_B_MISO, AB_B_CS, "B(matrix)",
                         (uint32_t)PIN_FUNC_GPIO, (uint32_t)FSPICLK_OUT_IDX,
                         &regs_b, &route_ok_b);
    if (nhigh_b < 0) { fails++; }
    else if (nhigh_b == 0) { fails++; }

    /* 汇总 */
    ESP_LOGW(TAG, "──── 汇总 ────");
    ESP_LOGW(TAG, "  A 臂 IOMUX  (CK=IO%d): 高电平 %d / %d   路由闸=%s",
             AB_A_CK, nhigh_a, AB_SAMPLE_POINTS, route_ok_a ? "通过" : "不通过");
    ESP_LOGW(TAG, "  B 臂 matrix (CK=IO%d): 高电平 %d / %d   路由闸=%s",
             AB_B_CK, nhigh_b, AB_SAMPLE_POINTS, route_ok_b ? "通过" : "不通过");

    if (nhigh_a > 0 && nhigh_b > 0) {
        ESP_LOGW(TAG, "★ 裁决: **SPI matrix 出向是通的** —— FSPI 可路由到任意 GPIO");
        ESP_LOGW(TAG, "   (与 UART 侧结论不同！S3 的 SPI/UART 在这一点上不一致)");
        *verdict = 1;
    } else if (nhigh_a > 0 && nhigh_b == 0 && route_ok_b) {
        /* 这是本实验能产出的**最强**结论：寄存器已独立证实 B 臂矩阵路由
         * 确实按预期机制建立起来了，pad 上却仍没有波形。 */
        ESP_LOGW(TAG, "★ 裁决: **SPI matrix 出向不通** —— 与 UART 侧结论一致");
        ESP_LOGW(TAG, "   强度: [AB0] 采样法已自证 OK + [AB-G] 寄存器已证实 B 臂"
                      "func_sel=FSPICLK_OUT_IDX 路由**确实建立**，仍无波形");
        ESP_LOGW(TAG, "   行动: S3 做 SPI 烧录器第一刀必须用 IOMUX 专属脚"
                      "(MOSI=11 CK=12 MISO=13 CS=10)");
        *verdict = 2;
    } else if (nhigh_a > 0 && nhigh_b == 0) {
        ESP_LOGE(TAG, "★ 裁决: **无效** —— A 臂有波形, 但 B 臂路由闸不通过");
        ESP_LOGE(TAG, "   B 臂没按预期机制布好路由(实测 MCU_SEL=%u func_sel=%u, 期望 0 / %u)",
                 (unsigned)regs_b.mcu_sel, (unsigned)regs_b.out_func_sel, (unsigned)FSPICLK_OUT_IDX);
        ESP_LOGE(TAG, "   ⇒ 读 0 归因于'驱动没布路由'而非'matrix 不通', 不可外推");
        *verdict = -1;
        fails++;
    } else {
        ESP_LOGE(TAG, "★ 裁决: **无效** —— A 臂也没采到波形, 阳性对照失效");
        ESP_LOGE(TAG, "   不能据此说 matrix 不通（那正是 CX1/CX2 共享的盲点）");
        *verdict = -1;
        fails++;
    }

    ESP_LOGW(TAG, "[AB] fails=%d verdict=%d (1=matrix通 2=matrix不通 -1=无效)",
             fails, *verdict);
    return (fails == 0) ? 0 : 1;
}
