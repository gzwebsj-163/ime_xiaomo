/* usbpd_bsp_probe.c —— SIM/REAL 结构性分水岭 + 分压还原往返 的独立口径探针
 *
 * ============================================================================
 * 为什么需要它 (不是为了凑测试数, 是因为它验的是本模块唯一承重的那条轴)
 * ----------------------------------------------------------------------------
 * usbpd 没有编译期模式开关 (无 MODE_OVERRIDE, 兄弟模块 9 个都有)。
 * 它的 SIM/REAL 隔离走【BSP 函数指针是否为 NULL】:
 *     hw_usbpd_is_sim()  == (s_bsp.analog_mv == NULL)
 * 与 hw_pin.c:615 `use_dev = (g_bsp.gpio_read == NULL)` 同形。
 *
 * ⇒ 对 usbpd 而言, 编译期模式对拍【结构性不适用】, 照抄兄弟模块会验到一个
 *   不存在的东西。真正该打的是 BSP 边界: 装/不装 BSP, 行为必须【可观测地不同】。
 *
 * 纪律 (锚点 L: 桩与被测物同型时, 对拍就是一台复印机):
 *   本探针 #include 真实头文件, 绝不自写一份同型声明。
 *   BSP 桩一律用【指定初始化器】( .analog_mv = ... ) 而非位置初始化,
 *   因为位置初始化一旦头文件加字段就会静默错位 —— 编译器对"按值初始化"最多
 *   告警不报错, 我曾因此把 3 个字段猜成 6 个。
 *
 * 纪律 (锚点: 断言失败先怀疑用例):
 *   ⚠️ 本探针第一版把【线电压 2700mV】直接塞进 analog_mv, 判 APPLE_2_4A 红。
 *      真因是量纲错: analog_mv 返回的是【TAP 原始读数】, sample() 才乘
 *      ratio(2960/1000) 还原成线电压。2700 原始 → 7992 线压 → 落表外。
 *      = 模块是对的, 用例是错的。这一节留在文件里, 免得下一个人再踩。
 *
 * 【独立口径】(为什么桩里不查 ratio_milli 反算 raw)
 *   若桩读 hw_usbpd_get_tap()->ratio_milli 反算 raw, 则"线电压→原始"这一步
 *   用的是被测物自己的常数, 被测物改了 ratio 两边一起错, 测试照样绿
 *   (锚点: 独立口径须在源码验证是否真独立)。
 *   ⇒ 本探针的 raw 是【写死的字面量】, 期望线电压也是【手算的字面量】,
 *     ratio 只由被测物使用。两端任一侧单独改动都会让断言变红。
 *
 * 判据:
 *   P1 未装 BSP  => is_sim()==1, sample() 诚实失败(负码), 不拿 0 冒充
 *   P2 装了 BSP  => is_sim()==0, 桩被调用, 原始→线电压还原正确, 判读正确
 *   P3 卸载回退  => 立刻回到 P1 的可观测状态(无残留)
 *   P4 悬空拾波闸 => 原始读数超轨 => 专用码 ERR_FLOAT + valid==0 (不判读)
 *   P5 底噪归一   => 原始读数低于底噪 => 归一为 0 而非判废 (BC1.2 判据 dm=0)
 *   交叉判据: P1 与 P2 的 sample() 返回码【必须不同】——
 *             若相同, 说明 REAL 分支根本没被走到, 探针是假阳性。
 * ============================================================================
 */
#include "hw_usbpd.h"      /* 锚定真头 —— 不用自写同型声明 */
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", (msg)); } \
    else { printf("  FAIL %s\n", (msg)); g_fail++; } \
} while (0)

/* ---- BSP 桩: 返回【TAP 原始读数】, 由用例写死 ---- */
static int g_adc_calls = 0;
static int g_raw_dp = 0, g_raw_dm = 0;

static int stub_analog_mv(int ch, int* out_mv)
{
    g_adc_calls++;
    if (!out_mv) return -1;
    /* ch: 0=DP, 1=DM (HW_USBPD_CH_DP / HW_USBPD_CH_DM) */
    *out_mv = (ch == HW_USBPD_CH_DP) ? g_raw_dp : g_raw_dm;
    return 0;
}

static hw_usbpd_bsp_t* bsp_new(void)
{
    static hw_usbpd_bsp_t bsp;
    /* 指定初始化器, 不用位置初始化 —— 见文件头纪律 */
    memset(&bsp, 0, sizeof(bsp));
    bsp.name      = "probe-stub";
    bsp.analog_mv = stub_analog_mv;
    return &bsp;
}

int main(void)
{
    hw_usbpd_dp_result_t r;
    int rc_sim, rc_real;

    printf("=== usbpd BSP 边界探针 (SIM/REAL 结构性分水岭 + 分压还原) ===\n\n");

    /* ---------- P1: 未装 BSP ---------- */
    printf("P1 未装 BSP:\n");
    hw_usbpd_bsp_install(NULL);
    CHECK(hw_usbpd_is_sim() == 1, "is_sim()==1 (未装 BSP 判为 SIM)");
    memset(&r, 0, sizeof(r));
    rc_sim = hw_usbpd_sample(&r);
    printf("       sample() rc=%d\n", rc_sim);
    /* 诚实失败: 负码。未装 BSP 时本层没有数据源, 拿 0 冒充就是假数据 */
    CHECK(rc_sim == HW_USBPD_ERR_NODEV,
          "sample() 返回 ERR_NODEV (分清'没装 BSP'与'硬件读失败')");

    /* ---------- P2: 装 BSP, 原始 912 → 线电压 2699 ---------- */
    /* 手算: 912 * 2960 / 1000 = 2699.52 → 整数除法取 2699
     * 落在 APPLE_2_4A 方框 dp[2650,2750] 内 */
    printf("P2 装 BSP (原始 912/912, 手算线电压 2699):\n");
    g_raw_dp = 912; g_raw_dm = 912; g_adc_calls = 0;
    hw_usbpd_bsp_install(bsp_new());
    CHECK(hw_usbpd_is_sim() == 0, "is_sim()==0 (装了 BSP 判为 REAL)");
    memset(&r, 0, sizeof(r));
    rc_real = hw_usbpd_sample(&r);
    printf("       sample() rc=%d raw=%d/%d line=%d/%d verdict=%d (%s) adc=%d\n",
           rc_real, r.dp_raw, r.dm_raw, r.dp_mv, r.dm_mv,
           (int)r.verdict, hw_usbpd_verdict_name(r.verdict), g_adc_calls);
    CHECK(rc_real == 0, "sample() 成功返回 (桩已提供数据源)");
    CHECK(g_adc_calls == 2, "桩被调用 2 次 (D+ 与 D- 各一次)");
    CHECK(r.dp_raw == 912 && r.dm_raw == 912, "原始读数原样回填");
    CHECK(r.dp_mv == 2699 && r.dm_mv == 2699,
          "分压还原正确: 912 → 2699mV (验证 ratio 往返, 非只看 verdict)");
    CHECK(r.verdict == HW_USBPD_APPLE_2_4A,
          "verdict==APPLE_2_4A (2699/2699 落在该行判读方框内)");
    CHECK(r.valid == 1, "valid==1 (通过悬空拾波闸, 本次读数有效)");

    /* ---------- 交叉判据: 两态必须不同 ---------- */
    printf("交叉判据:\n");
    CHECK(rc_sim != rc_real,
          "P1 与 P2 的 sample() 返回码不同 (若相同则 REAL 分支是死代码)");

    /* ---------- P3: 卸载回退 ---------- */
    printf("P3 卸载 BSP:\n");
    hw_usbpd_bsp_install(NULL);
    CHECK(hw_usbpd_is_sim() == 1, "卸载后 is_sim() 立刻回到 1 (无残留)");
    memset(&r, 0, sizeof(r));
    {
        int rc3 = hw_usbpd_sample(&r);
        CHECK(rc3 == rc_sim, "卸载后行为与 P1 逐位相同 (无状态残留)");
    }

    /* ---------- P4: 悬空拾波闸 (硬件坑①) ----------
     * raw_rail_mv=2850: 灌 3000 应被闸门拦下, 返回专用码而非继续判读 */
    printf("P4 悬空拾波闸 (原始 3000 > 轨 2850):\n");
    hw_usbpd_bsp_install(bsp_new());
    g_raw_dp = 3000; g_raw_dm = 912;
    memset(&r, 0, sizeof(r));
    {
        int rc4 = hw_usbpd_sample(&r);
        printf("       sample() rc=%d valid=%d\n", rc4, r.valid);
        CHECK(rc4 == HW_USBPD_ERR_FLOAT,
              "返回专用码 ERR_FLOAT (不是 ERR_PARAM: 调用方参数完全正确)");
        CHECK(r.valid == 0, "valid==0 (本次读数被判为无意义, 不产出 verdict)");
    }

    /* ---------- P5: 底噪归一 (硬件坑③) ----------
     * quiet_mv=30: 灌 20 应归一为 0, 而不是判废整次采样
     * (DCP_BC12 的判据就是 dm=0, 若判废会把自己杀死)
     * 手算: 202 * 2960 / 1000 = 597.92 → 597, 落在 BC1.2 方框 dp[550,650] */
    printf("P5 底噪归一 (D- 灌 20mV < 底噪 30, 手算线电压 0/597):\n");
    g_raw_dp = 202; g_raw_dm = 20;
    memset(&r, 0, sizeof(r));
    {
        int rc5 = hw_usbpd_sample(&r);
        printf("       sample() rc=%d raw=%d/%d line=%d/%d verdict=%d (%s)\n",
               rc5, r.dp_raw, r.dm_raw, r.dp_mv, r.dm_mv,
               (int)r.verdict, hw_usbpd_verdict_name(r.verdict));
        CHECK(rc5 == 0, "返回成功 (底噪归一而非判废 —— BC1.2 判据 dm=0 不能被自己杀死)");
        CHECK(r.dm_raw == 0 && r.dm_mv == 0, "D- 底噪归一为 0");
        CHECK(r.dp_mv == 597, "D+ 还原 597mV");
        CHECK(r.verdict == HW_USBPD_DCP_BC12, "verdict==DCP_BC12 (597/0)");
    }

    hw_usbpd_bsp_install(NULL);   /* 收尾: 卸干净, 别给后面留状态 */
    printf("\n%s  fails=%d\n",
           g_fail == 0 ? "RESULT: ALL PASS" : "RESULT: FAILED", g_fail);
    return g_fail;
}
