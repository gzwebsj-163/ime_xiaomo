/**
 * pin_g2_diag.c — SPI A/B 实验 [AB0] G2 失灵的**根因诊断矩阵**
 *
 * ══ 要解决的问题 ══
 * 2026-10-03 真机实测（boot_ab0.log）：
 *     [AB0] 采样法自证 FAIL  驱动低读到低 200/200, 驱动高读到高 0/200
 * 「低通高不通」这个**不对称**是唯一的线索。两个候选解释：
 *     (A) IO41 这只脚有外部接线/干扰（脚的问题）
 *     (B) 「配 output + 推挽驱动高 + 同脚读回」这个**方法**在本板读不到
 *         1（方法的问题，与脚无关）
 * 二者处置完全不同：(A) 换脚即可；(B) 整个采样法的阳性对照写法要重做。
 *
 * ══ 设计原则：把「方法」和「脚」彻底拆成两个变量 ══
 * 同一固件、同一批次、同一采样函数下，6 只脚 × 5 种方法 全排列。
 * 已知可用的方法作锚（pin_hw_prove_gpio 的弱上下拉，多轮实战通过），
 * 用它当标尺去量「推挽驱动高」这一路。
 *
 * ══ 5 种方法与期望值 ══
 *   T1  INPUT + 弱下拉, 不驱动        期望读到 低 200/200
 *   T2  INPUT + 弱上拉, 不驱动        期望读到 高 200/200   ← 已知可用（锚）
 *   T3  OUTPUT(IE关) 驱动 低          期望读到 低 200/200
 *   T4  OUTPUT(IE关) 驱动 高          期望读到 高 200/200   ← 本轮失败的那一路
 *   T5  INPUT_OUTPUT(IE开) 驱动 高    期望读到 高 200/200   ← 与 T4 唯一变量=FUN_IE
 *
 *   T4 与 T5 **只差 FUN_IE**。这是本次诊断的核心对照：
 *     gpio_get_level() 读什么，取决于 PIN 的输入使能(FUN_IE)：
 *       FUN_IE=0 → 读内部驱动值（应能读到 1）
 *       FUN_IE=1 → 读 pad 实际电平（脚悬空/被拉低则读 0）
 *   若 T4 全败而 T5 全过 ⇒ 根因是 **FUN_IE 语义**，与脚无关。
 *
 * ══ T6 跨脚金标准（不依赖「读自身驱动值」这个有歧义的操作）══
 *   脚 A 配 OUTPUT 驱动高，脚 B 配 INPUT 读。B 读到高 ⇒ 输出真的到了 pad。
 *   测 41→40 / 40→41 / 15→16 三对：分别覆盖「41 出」「41 入」「无关对照」。
 *   这一路无论 FUN_IE 语义如何都成立，是最硬的证据。
 *
 * ══ 两种读法口径互相对照 ══
 *   每项同时用 gpio_get_level() 和直读 GPIO.in 寄存器，输出 lvl 一致性。
 *   防「包装层坏了」这种自证盲点（参见本项目自写寄存器 dump 曾是坏工具的教训）。
 *
 * ══ 稳定性 ══
 *   整个矩阵跑 2 轮，判读器逐行比对两轮，偶发项无处藏身。
 *
 * 栈：一律 static（ MAIN_TASK_STACK_SIZE=3584，勿加栈数组）。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "pin_g2_diag.h"

static const char *TAG = "g2diag";

/* 6 只候选脚：刻意包含本轮失败脚 41、其档案伙伴 42，以及 4 只对照。
 * 已避开：LCD 4/5/6/7/10、wdbg 1/2/3、档案 8/9/11/12/13/14/18/21 的功能脚、
 *         PSRAM 占用 33~37、S3 strapping 0/3/45/46。 */
static const int PINS[6] = { 15, 16, 40, 41, 42, 21 };
#define NPIN 6
#define NSAMP 200
#define NRND  2      /* 轮数：2 轮互查偶发 */

/* ── 采样原语：两种读法口径 ───────────────────────────────────────────
 * 同一物理读数走两条互不依赖的路径，输出比对，避免「包装层自证盲点」。 */
typedef struct { int by_api; int by_reg; } lvl_t;

static inline uint32_t reg_rd(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }

/* S3 的 GPIO 分两组（由 gpio_reg.h 坐实，非记忆）：
 *   GPIO_IN    32 bit → 引脚 0..31      GPIO_ENABLE  32 bit → 0..31
 *   GPIO_IN1   22 bit → 引脚 32..53     GPIO_ENABLE1 22 bit → 32..53
 * ⚠️ 不存在 in2/enable2。IO41 落在 IN1 的第 (41-32)=9 位。 */
static inline int reg_in_bit(int p)
{
    uint32_t v = (p < 32) ? reg_rd(GPIO_IN_REG) : reg_rd(GPIO_IN1_REG);
    return (int)((v >> (p & 31)) & 1u);
}

static lvl_t read_lvl(int p)
{
    lvl_t v;
    v.by_api = gpio_get_level(p);
    v.by_reg = reg_in_bit(p);
    return v;
}

/* 采 n 点，统计「读到高」的份数。同时校验两条口径是否一致。 */
static int sample(int p, int n, int *out_mismatch)
{
    int hi = 0, i;
    for (i = 0; i < n; i++) {
        lvl_t v = read_lvl(p);
        if (v.by_api) hi++;
        if (v.by_api != v.by_reg) (*out_mismatch)++;
        esp_rom_delay_us(2);
    }
    return hi;
}

static void cfg_in(int p, int pull)
{
    gpio_config_t c;
    c.pin_bit_mask  = 1ULL << p;
    c.mode          = GPIO_MODE_INPUT;
    c.pull_up_en    = (pull == 1) ? GPIO_PULLUP_ENABLE  : GPIO_PULLUP_DISABLE;
    c.pull_down_en  = (pull == 2) ? GPIO_PULLDOWN_ENABLE: GPIO_PULLDOWN_DISABLE;
    c.intr_type     = GPIO_INTR_DISABLE;
    (void)gpio_config(&c);
    esp_rom_delay_us(3000);          /* 让弱上下拉在 pad 上建立 */
}

static void cfg_out(int p, int with_ie)
{
    gpio_config_t c;
    c.pin_bit_mask  = 1ULL << p;
    c.mode          = with_ie ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_OUTPUT;
    c.pull_up_en    = GPIO_PULLUP_DISABLE;
    c.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    c.intr_type     = GPIO_INTR_DISABLE;
    (void)gpio_config(&c);
}

/* 脚当前的寄存器快照：让「读到什么」与「配成了什么」同屏可比。
 * ⚠️ 刻意**不**读 SPIN.ie（本 IDF 未导出该结构体，且 MEMORY 记「自写寄存器
 * dump 曾是坏工具」）。FUN_IE 的效应由 T4 vs T5 的**行为**证明：
 * 已读 gpio_config() 源码坐实 —— mode 不含 INPUT 位时走 gpio_input_disable()，
 * 含时才 gpio_input_enable()。故 T4(IE关) 与 T5(IE开) 只差这一个位。 */
static uint32_t reg_snap(int p, uint32_t *fsel)
{
    /* FUNC0_OUT_SEL_CFG 在 0x554，FUNC1 在 0x558 ⇒ 步长 4 字节（由宏坐实） */
    *fsel = reg_rd(GPIO_FUNC0_OUT_SEL_CFG_REG + 4u * (uint32_t)p) & 0x3FFu;
    uint32_t v = (p < 32) ? reg_rd(GPIO_ENABLE_REG) : reg_rd(GPIO_ENABLE1_REG);
    return (v >> (p & 31)) & 1u;
}

/* ── 单脚 5 方法 ─────────────────────────────────────────────────── */
typedef struct {
    int t1_dn;    /* T1 读到低(期望 200) */
    int t2_up;    /* T2 读到高(期望 200) */
    int t3_lo;    /* T3 驱动低读到低(期望 200) */
    int t4_hi;    /* T4 OUTPUT 驱动高读到高(期望 200) */
    int t5_hi;    /* T5 INPUT_OUTPUT 驱动高读到高(期望 200) */
    int mism;     /* 两种读法口径不一致的次数 */
    uint32_t en; int fsel;
} row_t;

static row_t run_pin(int p)
{
    row_t r;
    int i, hi;
    r.mism = 0;

    /* T1 弱下拉：不驱动，期望 pad 被拉到低 */
    cfg_in(p, 2);
    hi = sample(p, NSAMP, &r.mism);
    r.t1_dn = NSAMP - hi;

    /* T2 弱上拉：不驱动，期望 pad 被拉到高 —— 已知可用，作锚 */
    cfg_in(p, 1);
    hi = sample(p, NSAMP, &r.mism);
    r.t2_up = hi;

    /* T3 OUTPUT 驱动低 */
    cfg_out(p, 0);
    gpio_set_level(p, 0); esp_rom_delay_us(2000);
    hi = sample(p, NSAMP, &r.mism);
    r.t3_lo = NSAMP - hi;

    /* T4 OUTPUT 驱动高 —— 本轮失败的那一路 */
    gpio_set_level(p, 1); esp_rom_delay_us(2000);
    hi = sample(p, NSAMP, &r.mism);
    r.t4_hi = hi;

    /* T5 INPUT_OUTPUT(FUN_IE=1) 驱动高 —— 与 T4 唯一变量 */
    cfg_out(p, 1);
    gpio_set_level(p, 1); esp_rom_delay_us(2000);
    hi = sample(p, NSAMP, &r.mism);
    r.t5_hi = hi;

    r.en = reg_snap(p, &r.fsel);

    /* 收尾：恢复为无上下拉的 input，不给下一脚留残留 */
    cfg_in(p, 0);
    (void)i;
    return r;
}

static void print_row(int rnd, int p, const row_t *r)
{
    /* 单行紧凑格式，便于外部判读器正则解析 */
    ESP_LOGW(TAG, "[G2D] R%d P%d t1dn=%d t2up=%d t3lo=%d t4hi=%d t5hi=%d mism=%d en=%u fsel=%u",
             rnd, p, r->t1_dn, r->t2_up, r->t3_lo, r->t4_hi, r->t5_hi,
             r->mism, (unsigned)r->en, (unsigned)r->fsel);
}

/* ── T6 跨脚金标准 ───────────────────────────────────────────────── */
static void cross(int rnd, int a, int b)
{
    int hi, mism = 0;
    cfg_in(b, 0);                 /* B 纯输入，无上下拉 */
    cfg_out(a, 0);                /* A 推挽输出，IE 关（与 T4 同设定） */
    gpio_set_level(a, 1);
    esp_rom_delay_us(5000);
    hi = sample(b, NSAMP, &mism);
    ESP_LOGW(TAG, "[G2D] R%d X d%dto%d=%d mism=%d", rnd, a, b, hi, mism);
    cfg_in(a, 0);
}

void pin_g2_diag_run(void)
{
    static row_t r[NRND][NPIN];   /* static：防栈帧炸弹 */
    int k, i;

    ESP_LOGW(TAG, "════ G2 根因诊断：方法 × 脚 交叉矩阵 ════");
    ESP_LOGW(TAG, "  6 脚 × 5 方法 + 3 对跨脚金标准，每项 %d 采样，跑 %d 轮", NSAMP, NRND);
    ESP_LOGW(TAG, "  T1 下拉→低 | T2 上拉→高(锚) | T3 OUT驱动低 | T4 OUT驱动高(失败路)");
    ESP_LOGW(TAG, "  T5 IN_OUT驱动高(与T4唯一变量=FUN_IE) | mism=两读法口径不一致数");
    ESP_LOGW(TAG, "  脚表: 15 16 40 41 42 21");

    for (k = 0; k < NRND; k++) {
        for (i = 0; i < NPIN; i++) {
            r[k][i] = run_pin(PINS[i]);
            print_row(k + 1, PINS[i], &r[k][i]);
        }
        cross(k + 1, 41, 40);
        cross(k + 1, 40, 41);
        cross(k + 1, 15, 16);
        ESP_LOGW(TAG, "[G2D] R%d --- 单脚矩阵结束，进入跨脚金标准 ---", k + 1);
    }

    ESP_LOGW(TAG, "════ 诊断矩阵结束 ════");
}
