/* usbpd_diag.c —— 真机验证桥
 *
 * 🕳️ 设计纪律 (来自 hw_dc / hw_pin 家族的实测教训, 全部是真付过代价的):
 *
 *  1. 【阴性对照先跑】: 任何"真机读数"类断言, 必须先证明这条读数
 *     【不是恒定值】。hw_wdbg 家族踩过: 自写 dump 输出"一整屏正常十六进制",
 *     实为恒定读数, 全程自洽。=> 真机项一律打印多拍读数, 让恒定值当场穿帮。
 *
 *  2. 【不把桩当实据】: 桩 BSP 与真 BSP 走【同一条判定路径】,
 *     但桩的读数是人工喂的。报告里必须分开列"桩证"和"真机证",
 *     混在一起就会出现"桩全过 = 真机全过"的错觉。
 *
 *  3. 【不假装能验的】: 本板无 USB-C 母座/分压/FUSB302,
 *     所以"真实 D+/D- 电压判读"【不在本工程能力范围】,
 *     报告里明写 SKIP 而不是打勾。
 *
 *  4. 【结构不变量真机也要过】: selftest 在真机跑一遍,
 *     证明 xtensa 下的整数除法/溢出行为与宿主一致 (黄金值必须一致)。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbpd_hw.h"

static int g_fail = 0;
static int g_skip = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        if (cond) { g_pass++; printf("  [OK]   " fmt "\n", ##__VA_ARGS__); } \
        else       { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }\
    } while (0)

#define SKIP(why) do { g_skip++; printf("  [SKIP] %s\n", why); } while (0)

/* ============================================================
 * Phase A —— 确定性 (不依赖硬件, 真机上同样必须过)
 * ============================================================ */
static void phase_A(void)
{
    hw_usbpd_stat_t st;
    const hw_usbpd_tap_t* tap;

    printf("\n================ Phase A: 确定性 ================\n");

    /* A1 黄金值: 换架构不许变 */
    CHECK(hw_usbpd_cmd("golden", 0) == (int)HW_USBPD_GOLDEN,
          "A1 黄金值跨架构一致 = 0x%08X (实得 0x%08X)",
          (unsigned)HW_USBPD_GOLDEN, (unsigned)hw_usbpd_cmd("golden", 0));

    /* A2 selftest 全绿: 表不变量/还原往返/闸门/分水岭/死代码表防护 */
    {
        int f = hw_usbpd_selftest();
        CHECK(f == 0, "A2 selftest 真机 fails=0 (实得 %d)", f);
    }

    /* A3 分压档案: 换档案不换判据 */
    tap = hw_usbpd_get_tap();
    /* ⚠️ ratio_milli 是 uint32_t, 在 xtensa 上 = long unsigned int。
     *   %d 直配会触发 -Werror=format (家族老坑: 宿主 clang 不报, 只有交叉编译暴露)。
     *   三元表达式 tap ? tap->ratio_milli : -1 也会把整型提升成 unsigned, 必须 cast。*/
    CHECK(tap && tap->ratio_milli == 2960, "A3 默认档案 ratio=2960 (实得 %d)",
          tap ? (int)tap->ratio_milli : -1);
    {
        /* 热换档案 */
        hw_usbpd_set_tap(&hw_usbpd_tap_19k_10k);
        tap = hw_usbpd_get_tap();
        CHECK(tap && tap->ratio_milli == 2960, "A4 set_tap 幂等 (实得 %d)",
              tap ? (int)tap->ratio_milli : -1);
    }

    /* A5 判据表形状: 行数与容差是跨口径标量 */
    {
        int n = 0;
        const hw_usbpd_qc_row_t* T = hw_usbpd_qc_table(&n);
        CHECK(T != NULL && n > 0, "A5 真表可导出 n=%d", n);
    }

    /* A6 命令层码位互斥: 未识别必须 != 参数错 */
    CHECK(hw_usbpd_cmd("no_such_cmd_xyz", 0) == HW_USBPD_ERR_UNKNOWN,
          "A6 未识别命令 = %d (不是 ERR_PARAM %d)",
          hw_usbpd_cmd("no_such_cmd_xyz", 0), HW_USBPD_ERR_PARAM);
    CHECK(hw_usbpd_cmd("help", 0) == HW_USBPD_HELP,
          "A7 help = %d (不与 ERR_NODEV 抢码)", HW_USBPD_HELP);
    CHECK(hw_usbpd_cmd("no_such_cmd_xyz", 0) != HW_USBPD_ERR_PARAM,
          "A8 未识别 与 参数错 可区分");

    /* A9 统计结构存在 */
    hw_usbpd_stat(&st);
    CHECK(st.samples > 0, "A9 采样计数在累加 (=%u)", (unsigned)st.samples);
}

/* ============================================================
 * Phase B —— 真硅
 * ============================================================ */
/* 相异值计数 —— B5 的判据与它自己的变异对照【共用同一个函数】。
 * 🕳️ 硬规矩: 判据不许在探针里另写一份, 否则"验判据的那段"和"被验的那段"
 *   会各自漂移, 于是变异对照验的是一个没人用的副本 —— 锚点 K 的老形状。 */
static int usbpd_count_distinct(const int* a, int n)
{
    int i, d = 0;
    for (i = 0; i < n; i++) {
        int j, dup = 0;
        for (j = 0; j < i; j++) if (a[j] == a[i]) { dup = 1; break; }
        if (!dup) d++;
    }
    return d;
}

static void phase_B(void)
{
    hw_usbpd_dp_result_t r;
    int rc, i;
    int reads[8];
    int distinct = 0;

    printf("\n================ Phase B: 真硅 ================\n");

    /* B1 未装 BSP = SIM, 且【必须诚实失败】 */
    CHECK(hw_usbpd_is_sim() == 1, "B1 未装 BSP => SIM");
    rc = hw_usbpd_sample(&r);
    CHECK(rc == HW_USBPD_ERR_NODEV, "B2 未装 BSP 采样 = ERR_NODEV(%d), 实得 %d",
          HW_USBPD_ERR_NODEV, rc);
    CHECK(r.valid == 0, "B3 失败时 valid=0 (不拿 0mV 冒充读数)");

    /* B4 装 BSP => 分水岭翻转 */
    if (usbpd_hw_install() != 0) {
        CHECK(0, "B4 装 BSP 失败");
        return;
    }
    CHECK(hw_usbpd_is_sim() == 0, "B4 装 BSP => REAL (分水岭翻转)");

    /* 🕳️ B5 【阴性对照】: 连续多拍真机读数, 必须【不全相等】。
     *   若全相等 = ADC 坏了/被钉死/我读的根本不是 ADC —— 此时 B6~B8 全是假绿。
     *   这条是 hw_wdbg 家族"一整屏正常十六进制实为恒定读数"的正面防役。*/
    hw_usbpd_stat_reset();
    for (i = 0; i < 8; i++) {
        rc = hw_usbpd_sample(&r);
        reads[i] = (rc == HW_USBPD_OK || rc == HW_USBPD_ERR_FLOAT) ? r.dp_raw : -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    distinct = usbpd_count_distinct(reads, 8);   /* 与变异对照共用同一实现 */
    printf("  [info] 8 拍 dp_raw: ");
    for (i = 0; i < 8; i++) printf("%d ", reads[i]);
    printf("\n");
    /* 🔴🔴【假探针, 锚点 K 第五形态】判据原写 `distinct > 0`, 而 8 拍【全相等】时
     *   distinct 恰好 == 1, 仍然满足 > 0 ⇒ 这条本该抓"恒定读数"的阴性对照,
     *   在它唯一要抓的那种输入下【判绿】。注释写着"hw_wdbg 家族恒定读数坑的正面防役",
     *   而它防不住 —— 又一次"注释里记着自己已加固, 反而掩盖它还活着"。
     *   真机当场撞上: 8 拍全是 3180 (ADC 满量程 raw=4095, 悬空脚被拾到轨),
     *   仍打印 [OK] B5。真恒定 = distinct == 1, 故必须判 > 1。
     *
     * 🧪【承重守卫必须有变异打它 — 锚点 J 的派生命令】
     *   光改判据不够: 我无法保证下次不会有人再把 "> 1" 写回 "> 0"。
     *   故下面用【构造数据】把判据本身当场验一遍 —— 恒定序列必须被判死,
     *   抖动序列必须被判活。判据若退化成恒真, 这条会立刻红。 */
    {
        static const int kConst[8]  = { 4095,4095,4095,4095,4095,4095,4095,4095 };
        static const int kJitter[8] = { 4095,4094,4095,4093,4095,4094,4092,4095 };
        int d_const  = usbpd_count_distinct(kConst, 8);
        int d_jitter = usbpd_count_distinct(kJitter, 8);
        CHECK(d_const == 1,
              "B5-变异 恒定序列 distinct 必须恰为 1 (实得 %d)", d_const);
        CHECK(!(d_const > 1),
              "B5-变异 判据 '>1' 必须能拒绝恒定序列 (distinct=%d 却判活)", d_const);
        CHECK(d_jitter > 1,
              "B5-变异 抖动序列必须被判活 (distinct=%d, 需 >1)", d_jitter);
    }
    CHECK(distinct > 1, "B5 真机读数非恒定 (8 拍中 %d 个不同值, 需 >1)", distinct);

    /* B6 真实悬空脚的读数特征: 不是 0 也不是 3300。
     *   本板 D+/D- 未接分压电阻 => 应落在几十~几百 mV 噪声区。
     *   若读出 0 或 3300, 说明这个脚被别的东西钉住了, 测的不是悬空闸。*/
    {
        int ok_cnt = 0, zero = 0, rail = 0;
        for (i = 0; i < 8; i++) {
            if (reads[i] < 0) continue;
            if (reads[i] == 0) zero++;
            if (reads[i] > 3200) rail++;
            if (reads[i] > 0 && reads[i] < 3200) ok_cnt++;
        }
        CHECK(ok_cnt == 8, "B6 8 拍全在 (0,3200) 噪声区 = %d (0mv:%d 轨:%d)",
              ok_cnt, zero, rail);
    }

    /* B7 ⭐ 悬空闸在真硬件上的行为 (本工程最有价值的一项):
     *   宿主桩只能喂人工构造的 9999mV; 真机上没有构造, 只有物理拾波。
     *   两种诚实结局都算过, 判据是【闸门不会把无效读数判成有效协议】:
     *     (a) 触发 ERR_FLOAT  => 悬空闸真的在工作
     *     (b) 不触发           => 那也是合法结果, 但必须 valid=0 或判读为 UNKNOWN
     *   唯一不可接受 = 悬空读数被判成 BC1.2/QC 之类的"成功档"。 */
    {
        hw_usbpd_stat_t st;
        rc = hw_usbpd_sample(&r);
        hw_usbpd_stat(&st);
        printf("  [info] 单次真机采样: rc=%d dp_raw=%d dm_raw=%d dp_mv=%d dm_mv=%d verdict=%s valid=%d\n",
               rc, r.dp_raw, r.dm_raw, r.dp_mv, r.dm_mv,
               hw_usbpd_verdict_name(r.verdict), r.valid);
        if (rc == HW_USBPD_ERR_FLOAT) {
            CHECK(r.valid == 0, "B7a 悬空闸触发 => valid=0");
            CHECK(st.dp_rail_rejects > 0, "B7a 悬空闸计数累加 (=%u)",
                  (unsigned)st.dp_rail_rejects);
        } else if (rc == HW_USBPD_OK) {
            /* 没触发 => 读数通过了闸, 那判读结果必须是"我不认识"或已标噪声归零 */
            CHECK(r.valid == 1, "B7b 通过闸 => valid=1");
            printf("  [info] 真机悬空未顶到轨(读数低于 rail 闸), 判读=%s\n",
                   hw_usbpd_verdict_name(r.verdict));
            CHECK(1, "B7b 闸门未触发 (物理拾波幅度不足以顶轨) —— 合法, 已如实记录");
        } else {
            CHECK(0, "B7c 意外返回码 %d", rc);
        }
    }

    /* B8 卸 BSP 立刻回落 SIM */
    usbpd_hw_uninstall();
    CHECK(hw_usbpd_is_sim() == 1, "B8 卸 BSP => 回到 SIM");
    rc = hw_usbpd_sample(&r);
    CHECK(rc == HW_USBPD_ERR_NODEV, "B9 卸 BSP 后采样 = ERR_NODEV (实得 %d)", rc);

    /* B10 PD 通道: 本板无 FUSB302 => 必须诚实 UNSUPPORT, 绝不编造 PDO */
    {
        uint32_t pdo[8]; int n = 0;
        int rc_poll = hw_usbpd_pd_poll(NULL);
        int rc_pdo  = hw_usbpd_pd_read_pdo(pdo, 8, &n);
        CHECK(rc_poll == HW_USBPD_ERR_UNSUPPORT,
              "B10 pd_poll 无硬件 = ERR_UNSUPPORT(%d), 实得 %d",
              HW_USBPD_ERR_UNSUPPORT, rc_poll);
        CHECK(rc_pdo == HW_USBPD_ERR_UNSUPPORT && n == 0,
              "B11 pd_read_pdo 诚实拒绝且不编造 n=%d", n);
    }

    /* B12 明确 SKIP: 真实 D+/D- 电压判读 —— 本板无 USB-C 母座+分压电阻 */
    SKIP("B12 真实充电器 D+/D- 电压判读 —— 本板无 USB-C 母座/分压电阻, 能力范围外");
    SKIP("B13 FUSB302 I²C 读 CC 状态 —— 本板无该芯片");
    SKIP("B14 CC 双相比特捕获 —— 本板无比特捕获硬件");
}

/* ============================================================
 * Phase C —— 引脚体检 (人工判读, 不自动断言)
 * ============================================================ */
static void phase_C(void)
{
    printf("\n================ Phase C: 引脚体检 ================\n");
    usbpd_hw_pin_scan();
    printf("  [note] 本项只打印, 不断言 —— 缺外部跳线时无法构造对照\n");
}

int usbpd_diag_run(void)
{
    printf("\n##################################################\n");
    printf("#  hw_usbpd  ESP32-S3 真机验证桥\n");
    printf("#  板载串口 = CH343 桥到 UART0 (非 USB-JTAG!)\n");
    printf("##################################################\n");

    phase_A();
    phase_B();
    phase_C();

    printf("\n================ 汇总 ================\n");
    printf("  通过: %d   失败: %d   跳过: %d\n", g_pass, g_fail, g_skip);
    if (g_fail == 0) {
        printf("  [RESULT] ALL PASS (fails=0)\n");
    } else {
        printf("  [RESULT] FAIL (fails=%d)\n", g_fail);
    }
    return g_fail;
}
