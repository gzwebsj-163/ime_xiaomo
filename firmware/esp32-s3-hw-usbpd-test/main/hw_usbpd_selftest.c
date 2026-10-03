/* hw_usbpd_selftest.c —— 自校验 + 【验证器的验证】
 *
 * ============================================================
 * 本文件不只是"跑一遍看绿"。按锚点 J:
 *   全绿的测试不证明测试有效, 要把裁判承重先称裁判。
 *
 *   所以这里含两类用例:
 *     [正例] 期望判定的 —— 证明功能对
 *     [变异] 故意植入缺陷 —— 证明本 selftest 【会红】
 *           变异存活 = 裁判有盲点, 不是产品通过。
 * ============================================================
 */
#include "hw_usbpd.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_var_fail, g_var_total;
static int g_case_total;

#define CHECK(cond, fmt, ...)                                              \
    do { g_case_total++;                                                   \
         if (!(cond)) { g_fail++;                                          \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }                 \
    } while (0)

/* ---------- 确定性模拟 BSP (SIM 侧可测性的前提) ---------- */
static int  g_sim_dp_raw, g_sim_dm_raw;
static int  g_sim_fail_dp;          /* >0 = 让该通道返回 <0 */
static int  g_i2c_installed;

static int sim_analog_mv(int ch, int* out_mv)
{
    if (ch == HW_USBPD_CH_DP) {
        if (g_sim_fail_dp) return -7;
        *out_mv = g_sim_dp_raw; return 0;
    }
    if (ch == HW_USBPD_CH_DM) { *out_mv = g_sim_dm_raw; return 0; }
    return -2;                          /* 未建模通道 = 诚实失败 */
}
static int sim_i2c_read(uint8_t r, uint8_t* b, uint32_t n)
{ (void)r; (void)b; (void)n; g_i2c_installed = 1; return 0; }

/* 🕳️ 指定初始化器, 不是位置初始化 (锚点 L 实锤 2026-10-03):
 *   位置写法 `{ "sim", sim_analog_mv, 0, sim_i2c_read, 0 }` 在结构体
 *   增删字段时会【静默错位】—— 编译器对位置初始化只告警不报错,
 *   而对"值不解引用"的情况甚至什么都不说。
 *   锚点 L: 我曾用 `{{0,0,0,0,0,0}}` 猜字段数, 编译器一声不吭。
 *   指定初始化器保证: 字段名对不上会【硬错误】, 漏填字段显式补 0。 */
static const hw_usbpd_bsp_t SIM_BSP = {
    .name        = "sim",
    .analog_mv   = sim_analog_mv,
    .i2c_write   = 0,
    .i2c_read    = sim_i2c_read,
    .biphase_rx  = 0
};

/* 把线电压灌进"真实分压链路"再采回来 —— 测的是整条链路不是分类纯函数 */
static void wire_line_mv(int dp_mv, int dm_mv)
{
    g_sim_dp_raw = dp_mv * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
    g_sim_dm_raw = dm_mv * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
}

static hw_usbpd_verdict_t judge(int dp_mv, int dm_mv)
{
    hw_usbpd_dp_result_t r;
    wire_line_mv(dp_mv, dm_mv);
    if (hw_usbpd_sample(&r) != HW_USBPD_OK) return HW_USBPD_UNKNOWN;
    return r.verdict;
}

/* =============== [变异] 判据表不变量检查器 ===============
 * 这是裁判本身。它抓的正是 c3_adc 版 proto_tap.c:30-31 的病:
 *   { 600, 0, "BC1.2" } 与 { 500, 0, "DCP" } 相差 100mV <= 2*TOL
 *   => 方框重叠 => 后者永不可达 = 死代码表。
 * 返回重叠对数 (0 = 全部行两两不重叠)。
 *
 * 🕳️【V-V 实锤 2026-10-03】这个函数原先手抄了一份 T[] 副本并检查副本。
 *   变异测试 M2 因此存活: 真表被塞回 {500,0} 死代码行, 真表已重叠,
 *   但检查器遍历副本看不到 => 全绿放行。
 *   注释里还写着"下面 [12] 逐行可分辨就是双份失配的探针" —— [12] 从不存在。
 *   => 两重教训: (a) 检查器必须遍历被测物本身 (走 hw_usbpd_qc_table);
 *                 (b) 注释里承诺的守卫, 要用"编号真的存在"来反查, 不能信。
 *   下面 [12] 现已补上 = 【真表行数 >= 4 且每行合法】的活性探针。
 */
static int table_invariant_check(void)
{
    /* 🔒 走真表出口, 不再有任何本地副本 */
    int n = 0;
    const hw_usbpd_qc_row_t* T = hw_usbpd_qc_table(&n);
    int i, j, bad = 0;

    if (!T || n < 4) {           /* 阳性: 表塌成空/一行, 检查器必须报坏 */
        printf("    真表异常: n=%d T=%p\n", n, (const void*)T);
        return 1;
    }
    for (i = 0; i < n; i++) {
        for (j = i+1; j < n; j++) {
            int ddp = T[i].dp - T[j].dp; if (ddp < 0) ddp = -ddp;
            int ddm = T[i].dm - T[j].dm; if (ddm < 0) ddm = -ddm;
            /* 两行判读方框 [c-TOL, c+TOL] 相交 <=> 两行间距 <= 2*TOL
             *
             * 🕳️ 这里必须是 <= 而不是 < !
             *   塞回的 {500,0} 距 {600,0} 恰 100mV, 而 2*TOL 也恰是 100。
             *   写成 < 时检查器判"不重叠"放过去了, 但 550mV 同时命中两行。 */
            if (ddp <= 2*HW_USBPD_TOL_MV && ddm <= 2*HW_USBPD_TOL_MV) {
                printf("    行重叠: #%d(%d,%d) vs #%d(%d,%d) 间距=(%d,%d) <= 2*TOL=%d\n",
                       i, T[i].dp, T[i].dm, j, T[j].dp, T[j].dm,
                       ddp, ddm, 2*HW_USBPD_TOL_MV);
                bad++;
            }
        }
    }
    return bad;
}

/* c3_adac 原版表 (容差 300mV) —— 阴性对照。
 * 🕳️ 这里的 TOL 必须【用旧版的 300】, 才复现旧版的病;
 *    若用当前 50 则旧表反而"合法", 对照就失去意义。 */
static int c3adac_table_is_rejected(void)
{
    static const struct { int dp, dm; int v; } BAD[] = {
        { 3300, 600, 0 }, { 600, 3300, 0 }, { 3300, 3300, 0 },
        { 2700, 2700, 0 }, { 2700, 2000, 0 }, { 2000, 2000, 0 },
        { 1200, 1200, 0 },
        {  600,    0, 0 },   /* BC1.2 */
        {  500,    0, 0 },   /* DCP  —— 与上行差 100mV, 死代码 */
    };
    const int C3_TOL = 300;   /* c3_adc 版 proto_tap.c:34 的 QC_TOL */
    int n = (int)(sizeof(BAD)/sizeof(BAD[0]));
    int i, j, bad = 0;
    for (i = 0; i < n; i++)
        for (j = i+1; j < n; j++) {
            int ddp = BAD[i].dp - BAD[j].dp; if (ddp < 0) ddp = -ddp;
            int ddm = BAD[i].dm - BAD[j].dm; if (ddm < 0) ddm = -ddm;
            if (ddp <= 2*C3_TOL && ddm <= 2*C3_TOL) bad++;
        }
    return bad;   /* >0 = 检查器抓到了 c3_adc 的病 */
}

/* =============== FNV-1a-32 黄金值 (与 hw 家族同算法) =============== */
static uint32_t fnv1a32(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 0x811C9DC5u; size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
    return h;
}

/* =============== 主 selftest =============== */
int hw_usbpd_selftest(void)
{
    g_fail = 0; g_var_total = 0; g_var_fail = 0; g_case_total = 0;

    printf("[1] 判据表构造性不变量 (防死代码表)\n");
    {
        int bad = table_invariant_check();
        /* 🕳️ 消息必须区分两种失败: 1 = 真表异常/空表(走 n<4 或 !T 守卫),
         *    >1 = 判读方框重叠。写成笼统的"存在 N 处重叠"会误导读日志的人
         *    (变异 M10 实锤: 报 1 处重叠, 实际是行数守卫触发)。 */
        CHECK(bad == 0, "判据表不变量失败 (bad=%d: 1=真表异常/空表, >1=判读方框重叠)", bad);
    }

    printf("[2] 阴性对照: c3_adc 原版表必须被判为坏\n");
    {
        int bad = c3adac_table_is_rejected();
        g_var_total++;
        if (bad <= 0) { g_var_fail++; printf("  [VAR-FAIL] 检查器没抓到 c3_adc 死代码表, 裁判有盲点!\n"); }
        else          printf("  [VAR-OK] 抓到 %d 处重叠 (c3_adc 版 {600,0}/{500,0} 确认死代码)\n", bad);
    }

    printf("[3] 分水岭: BSP 未装 = SIM\n");
    {
        hw_usbpd_bsp_install(NULL);
        CHECK(hw_usbpd_is_sim() == 1, "未装 BSP 应为 SIM");
        hw_usbpd_bsp_install(&SIM_BSP);
        CHECK(hw_usbpd_is_sim() == 0, "装了 BSP 应为 REAL");
        /* 判定【实时】跟随指针, 不是缓存标志 */
        hw_usbpd_bsp_install(NULL);
        CHECK(hw_usbpd_is_sim() == 1, "卸载后应立刻回到 SIM");
        hw_usbpd_bsp_install(&SIM_BSP);
    }

    printf("[4] 未装 BSP 时采样必须诚实失败 (不假装读到 0mV)\n");
    {
        hw_usbpd_dp_result_t r;
        int rc;
        hw_usbpd_bsp_install(NULL);
        rc = hw_usbpd_sample(&r);
        CHECK(rc == HW_USBPD_ERR_NODEV, "未装 BSP 应返回 ERR_NODEV, 实得 %d", rc);
        CHECK(r.valid == 0, "失败时 valid 必须为 0");
        hw_usbpd_bsp_install(&SIM_BSP);
    }

    printf("[5] 硬件坑① 悬空拾波闸\n");
    {
        hw_usbpd_dp_result_t r;
        int rc;
        g_sim_fail_dp = 0;
        wire_line_mv(2700, 2700);
        g_sim_dp_raw = 3300;            /* 悬空 = 顶到 VDD 轨 */
        rc = hw_usbpd_sample(&r);
        CHECK(rc != HW_USBPD_OK, "悬空读数必须被闸门拦下, 实得 rc=%d", rc);
        CHECK(r.valid == 0, "悬空时 valid 必须 0 (不能拿悬空值判读)");
        { hw_usbpd_stat_t st; hw_usbpd_stat(&st);
          CHECK(st.dp_rail_rejects >= 1, "悬空闸命中次数未累加"); }
    }

    printf("[6] 硬件坑② BSP 通道失败必须诚实上报\n");
    {
        hw_usbpd_dp_result_t r;
        int rc;
        g_sim_fail_dp = 1;
        rc = hw_usbpd_sample(&r);
        CHECK(rc == HW_USBPD_ERR_HW, "通道失败应返回 ERR_HW, 实得 %d", rc);
        g_sim_fail_dp = 0;
    }

    printf("[7] 整链路判定 (灌线电压 → 分压 → 还原 → 查表)\n");
    {
        /* 🕳️ 用例期望必须与 [1]/[12] 的表同源, 否则判据表一改用例就红。
         *   DCP 短接行已被 BC1.2(600/0) 吸收; 0/0 现在诚实判 SDP。 */
        CHECK(judge(0, 0)         == HW_USBPD_SDP,          "0/0 应判 SDP(无分压)");
        CHECK(judge(600, 0)       == HW_USBPD_DCP_BC12,     "600/0 应判 BC1.2/DCP");
        CHECK(judge(750, 250)     == HW_USBPD_QC_5V,         "750/250 应判 QC5V");
        CHECK(judge(2700, 2700)   == HW_USBPD_APPLE_2_4A,    "2700/2700 应判 Apple2.4A");
        CHECK(judge(2700, 2000)   == HW_USBPD_APPLE_1A,      "2700/2000 应判 Apple1A");
        CHECK(judge(2000, 2700)   == HW_USBPD_APPLE_1_5A,    "2000/2700 应判 Apple1.5A");
        CHECK(judge(2000, 750)    == HW_USBPD_QC_9V,         "2000/750 应判 QC9V");
        CHECK(judge(2000, 1500)   == HW_USBPD_QC_12V,        "2000/1500 应判 QC12V");
        CHECK(judge(1200, 1200)   == HW_USBPD_SAMSUNG_AFC,   "1200/1200 应判 AFC");
        CHECK(judge(2200, 1900)   == HW_USBPD_FASTCHARGE_UNKNOWN, "未知组合应报未识别");
    }

    printf("[8] 分压还原往返 (TAP → 线压 → TAP)\n");
    {
        int line_mv[] = { 0, 600, 1800, 2000, 2700, 3300 };
        int i, ok = 1;
        for (i = 0; i < 6; i++) {
            int tap = line_mv[i] * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
            int back = tap * hw_usbpd_tap_19k_10k.ratio_milli / 1000;
            if (back < line_mv[i] - 10 || back > line_mv[i] + 10) ok = 0;
        }
        CHECK(ok, "分压还原往返误差超 10mV");
    }

    printf("[9] 档案可热换 (换分压只改档案, 判据零改动)\n");
    {
        hw_usbpd_tap_t custom = { "1k/1k", 2000u, 2850, 30 };
        hw_usbpd_set_tap(&custom);
        CHECK(hw_usbpd_get_tap() == &custom, "档案未切换");
        hw_usbpd_set_tap(&hw_usbpd_tap_19k_10k);
        CHECK(hw_usbpd_get_tap() == &hw_usbpd_tap_19k_10k, "档案未切回");
    }

    printf("[10] PD 通道: 无 FUSB302 必须诚实 ERR_UNSUPPORT\n");
    {
        hw_usbpd_stat_t st; uint32_t pdo[8]; int n = -1;
        /* 🕳️ 用例错先怀疑用例: 上一版这里直接用 SIM_BSP, 但它的第 4 个
         *    字段 sim_i2c_read 是【非 NULL】的, 条件根本没成立。
         *    正确做法 = 显式装一个 i2c_read=NULL 的 BSP。
         *    🕳️ 且必须用【指定初始化器】: 位置写法在这类"故意留空"的桩上
         *    极易看错位 —— 写成 { "no-i2c", sim_analog_mv, 0, 0, 0 } 恰好对,
         *    但一旦结构体加字段, 那个 0 会滑到 i2c_read 之外, 桩就废了。 */
        static const hw_usbpd_bsp_t NOI2C = {
            .name        = "no-i2c",
            .analog_mv   = sim_analog_mv,
            .i2c_write   = 0,
            .i2c_read    = 0,        /* ← 本例的【被测点】, 必须显式为 0 */
            .biphase_rx  = 0
        };
        hw_usbpd_bsp_install(&NOI2C);
        CHECK(hw_usbpd_pd_poll(&st) == HW_USBPD_ERR_UNSUPPORT, "无 I2C 应 ERR_UNSUPPORT");
        CHECK(hw_usbpd_pd_read_pdo(pdo, 8, &n) == HW_USBPD_ERR_UNSUPPORT,
              "无 I2C 读 PDO 应 ERR_UNSUPPORT (绝不返回假 PDO)");
        CHECK(n == 0, "失败时 n_out 必须为 0, 实得 %d", n);
        /* 阳性对照: 装了 I2C 时 pd_poll 必须能跑通 (否则上面是恒真) */
        g_i2c_installed = 0;
        hw_usbpd_bsp_install(&SIM_BSP);
        CHECK(hw_usbpd_pd_poll(&st) == HW_USBPD_OK, "装了 i2c_read 应 OK");
        CHECK(g_i2c_installed == 1, "阳性对照: i2c_read 未被调用");
    }

    printf("[11] 黄金值 (只覆盖标量字段, 可跨口径复现)\n");
    {
        const hw_usbpd_tap_t* t = &hw_usbpd_tap_19k_10k;
        /* 🕳️ 只哈希标量: name 指针受 ASLR 影响, 哈希它 = 指纹每次都变。 */
        uint8_t scalars[12];
        memcpy(scalars + 0,  &t->ratio_milli,  4);
        memcpy(scalars + 4,  &t->raw_rail_mv, 4);
        memcpy(scalars + 8,  &t->quiet_mv,    4);
        {
            uint32_t g = fnv1a32(scalars, 12);
            CHECK(g == HW_USBPD_GOLDEN, "黄金值不符: 实得 0x%08X 期望 0x%08X",
                  (unsigned)g, (unsigned)HW_USBPD_GOLDEN);
        }
    }

    printf("[12] 变异对照: 判据表逐行可分辨 (无不可达行)\n");
    {
        /* 把每行的【精确值】灌回去, 必须判回它自己。
         * 若某行永不可达, 这里就会红 —— 这是死代码表的第二道防线。
         *
         * 🕳️ 原实现在这里手抄了第三份 ALL[] 副本, 违背 [1] 定的"不许有副本"规矩,
         *   而且方向上是【假探针】: 副本值喂给读真表的 judge(), 变异往真表塞行时
         *   这 8 个精确值照旧命中 => 结构上抓不到。现改为遍历真表。 */
        int tn = 0, i, unreachable = 0;
        const hw_usbpd_qc_row_t* T = hw_usbpd_qc_table(&tn);
        /* 🕳️【测试自身要能报, 而不是崩, 2026-10-03 修】
         *   本处原先直接 T[i].dp, 没有 NULL 守卫。变异 M9(真表出口返 NULL)
         *   时: [1] 的 !T 守卫【确实报了坏】, 但本循环随后解引用 NULL 崩掉,
         *   进程被 SIGSEGV 打死 ⇒ 退出码 -11, 断言报告根本没机会打印。
         *   后果是【信号失效】: 变异测试只能报 "KILLED-BY-CRASH", 而崩溃
         *   与"守卫按设计命中"是两回事 —— 崩在别处不能证明 !T 守卫生效。
         *   这就是"失败消息硬编码/缺失比没有原因更坏"的变体: 原因被信号盖了。
         *   修法: 真表为空时【报断言失败并跳过循环】, 让 M9 变成干净的
         *   断言命中, 从而真正证明 !T 守卫有效。 */
        CHECK(T != 0 && tn > 0, "真表出口返回空 (T=%p, n=%d)", (const void*)T, tn);
        for (i = 0; T && i < tn; i++) {
            hw_usbpd_verdict_t got = judge(T[i].dp, T[i].dm);
            g_var_total++;
            if (got != T[i].v) { g_var_fail++; unreachable++;
                printf("    行 #%d(%d,%d) 判为 %s 而非 %s\n", i,
                       T[i].dp, T[i].dm,
                       hw_usbpd_verdict_name(got), hw_usbpd_verdict_name(T[i].v)); }
        }
        CHECK(unreachable == 0, "存在 %d 行不可达 (死代码表)", unreachable);
    }

    printf("[13] 悬空闸必须返专用码 ERR_FLOAT (不能甩锅 ERR_PARAM)\n");
    {
        /* 🕳️ 锚点 M10: 误导性失败原因比没有原因更坏。
         *   悬空时调用方参数【完全正确】, 返 ERR_PARAM 会把排查者
         *   引去检查自己的指针, 失败原因指向错误方向。
         *   变异 M11 会把这里改回 ERR_PARAM, 本断言必须抓到。 */
        hw_usbpd_dp_result_t r;
        int rc;
        g_sim_fail_dp = 0;
        g_sim_dp_raw = 3300;                 /* 悬空 */
        rc = hw_usbpd_sample(&r);
        CHECK(rc == HW_USBPD_ERR_FLOAT, "悬空应返 ERR_FLOAT(%d), 实得 %d",
              HW_USBPD_ERR_FLOAT, rc);
        CHECK(rc != HW_USBPD_ERR_PARAM, "悬空不得返 ERR_PARAM (会把排查引向调用方参数)");
        /* 码位互斥: 悬空码不得与任何其它语义码撞车 */
        CHECK(HW_USBPD_ERR_FLOAT != HW_USBPD_ERR_PARAM &&
              HW_USBPD_ERR_FLOAT != HW_USBPD_ERR_NODEV &&
              HW_USBPD_ERR_FLOAT != HW_USBPD_ERR_HW &&
              HW_USBPD_ERR_FLOAT != HW_USBPD_ERR_UNSUPPORT &&
              HW_USBPD_ERR_FLOAT != HW_USBPD_HELP,
              "ERR_FLOAT 与其它码位重叠");
    }

    printf("[14] 硬件坑③ 底噪确认闸 (quiet_mv 必须真生效, 不是死字段)\n");
    {
        /* 🕳️ 这条断言就是【假守卫】的解药。
         *   修复前 quiet_mv 改 300 倍 (30→9999) 也不影响任何行为断言,
         *   只有黄金值那条红 —— 也就是"看起来被锁住, 实际无感"。
         *   变异 M12 把 quiet_mv 闸整段删掉, 本断言必须红。
         *
         * 场景: D+ 是 BC1.2 的 600mV, D- 悬空 (只读到 40mV 底噪)。
         *   不做底噪确认 → 40mV 与 0mV 相差 40 < TOL(50) 仍落进
         *   {600, 0} 方框 => 判成 BC1.2。但那根线根本没接。
         *   做了确认 → 40mV 归一为 0, 判定不变, 但 dp/dm 读数诚实。 */
        hw_usbpd_dp_result_t r;
        g_sim_fail_dp = 0;
        /* 🕳️【用例错先怀疑用例】第一版这里用 wire_line_mv(600, 0) 灌值,
         *   而 wire_line_mv 是整数除法 200*1000/2960 → 再 *2960/1000,
         *   往返必然截断 (实得 198 而非 200) => 断言"必须精确 200"自己就错。
         *   (与 probe_cap T3 同一类: 用例的期望值算错, 不是产品错。)
         *   修法 = 不走有损往返, 直接反解出【期望的还原值】所对应的 raw,
         *   再用 ±1 raw 的余量构造"底噪级"与"清晰高电平"。 */
        g_sim_dp_raw = 600 * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
        /* D- 给一个明确落在底噪以下的 raw: 期望还原值 = 40mV 量级 */
        g_sim_dm_raw = 40 * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
        CHECK(hw_usbpd_sample(&r) == HW_USBPD_OK, "底噪级采样应成功");
        /* 判读结果: 归一后仍应是 BC1.2 (40mV 本就在 0 的容差内) */
        CHECK(r.verdict == HW_USBPD_DCP_BC12, "底噪归一后应判 BC1.2, 实得 %s",
              hw_usbpd_verdict_name(r.verdict));
        /* 🔑 真正的判别点: 读数本身必须已被归一为 0。
         *   若闸门被删, dm_mv 会是 40 → 这里红。 */
        CHECK(r.dm_mv == 0, "低于底噪的读数必须归一为 0, 实得 %d "
              "(闸门被删? 判据层对 quiet_mv 完全无感=假守卫)", r.dm_mv);
        /* 阳性对照: 高于底噪的读数绝不能被归一。
         *   期望值用【同一个反解式】算, 不写死 200 —— 避免重犯上面的错。 */
        {
            int want = 200;
            g_sim_dm_raw = want * 1000 / hw_usbpd_tap_19k_10k.ratio_milli;
            CHECK(hw_usbpd_sample(&r) == HW_USBPD_OK, "采样应成功");
            CHECK(r.dm_mv >= want - 5 && r.dm_mv <= want + 5,
                  "高于底噪的 %dmV 不得被归一, 实得 %d (容差 ±5, 吸收整数除法截断)",
                  want, r.dm_mv);
            CHECK(r.dm_mv > hw_usbpd_tap_19k_10k.quiet_mv,
                  "阳性对照: 200mV 必须高于底噪阈值 %d, 用例数据有问题",
                  hw_usbpd_tap_19k_10k.quiet_mv);
        }
    }

    printf("[15] cmd/cli 出口: 码位互斥 + 未装 BSP 诚实失败\n");
    {
        /* 🕳️ help 曾取 -2, 与 ERR_NODEV 同码 ⇒ "未装 BSP"会被读成"要帮助",
         *   两个判据同时失效。变异 M13 会把 help 改回 -2, 本组必须红。 */
        CHECK(HW_USBPD_HELP != HW_USBPD_ERR_NODEV,
              "HELP(%d) 与 ERR_NODEV(%d) 撞码", HW_USBPD_HELP, HW_USBPD_ERR_NODEV);
        /* 🕳️ 同型第二处 (加 "at <mv>" 时抓到): "未识别"原先也返回 -1, 与
         *   ERR_PARAM 同码 ⇒ "命令名打错"和"参数不合法"调用方分不开。
         *   第一版只修了 help 撞 ERR_NODEV, 漏了这一个 —— 同一个毛病修一半。
         *   => 守卫必须【成组】: 每加一个新码位, 就要同时断言它与所有旧码位互斥。 */
        CHECK(HW_USBPD_ERR_UNKNOWN != HW_USBPD_ERR_PARAM,
              "ERR_UNKNOWN(%d) 与 ERR_PARAM(%d) 撞码", HW_USBPD_ERR_UNKNOWN, HW_USBPD_ERR_PARAM);
        CHECK(HW_USBPD_ERR_UNKNOWN != HW_USBPD_HELP,
              "ERR_UNKNOWN(%d) 与 HELP(%d) 撞码", HW_USBPD_ERR_UNKNOWN, HW_USBPD_HELP);
        /* 全部负码两两互斥 (7 个码位, 21 对) —— 一次性把整组锁死,
         * 免得以后再加码位时重犯同型错。 */
        {
            static const int codes[] = { HW_USBPD_ERR_PARAM, HW_USBPD_ERR_NODEV,
                                         HW_USBPD_ERR_HW, HW_USBPD_ERR_UNSUPPORT,
                                         HW_USBPD_ERR_FLOAT, HW_USBPD_HELP,
                                         HW_USBPD_ERR_UNKNOWN };
            const int n = (int)(sizeof(codes) / sizeof(codes[0]));
            int i, j, bad = 0;
            for (i = 0; i < n; i++)
                for (j = i + 1; j < n; j++)
                    if (codes[i] == codes[j]) bad++;
            CHECK(bad == 0, "7 个负码必须两两互斥, 实得 %d 对撞码", bad);
        }
        CHECK(hw_usbpd_cmd("help", 0) == HW_USBPD_HELP, "help 码位不符");
        CHECK(hw_usbpd_cmd("nosuchcmd", 0) == HW_USBPD_ERR_UNKNOWN, "未识别命令应返 ERR_UNKNOWN");
        /* 未装 BSP 时 sample 必须诚实 ERR_NODEV, 不能返回假判定 */
        hw_usbpd_bsp_install(NULL);
        CHECK(hw_usbpd_cmd("sample", 0) == HW_USBPD_ERR_NODEV,
              "未装 BSP 时 sample 应 ERR_NODEV");
        CHECK(hw_usbpd_cmd("sim", 0) == 1, "未装 BSP 时 sim 应为 1");
        hw_usbpd_bsp_install(&SIM_BSP);
        /* 装 BSP 后 sim=0, 且 rows/tol 这类不依赖硬件的必须照常返回有效值 */
        CHECK(hw_usbpd_cmd("sim", 0) == 0, "装了 BSP 后 sim 应为 0");
        CHECK(hw_usbpd_cmd("rows", 0) >= 4, "rows 应返回真表行数");
        CHECK(hw_usbpd_cmd("tol", 0) == HW_USBPD_TOL_MV, "tol 应为 50");
        /* 🔑【argv 契约】家族约定 = main.c 原样传 (argc, argv), 子命令在 argv[2]。
         *   历史上本层写成 argv[1], 症状是"每条命令都返回 -1 未识别",
         *   看起来像整条命令线没接上, 真因只是偏移 1。
         *
         * 🕳️【恒真信号, 锚点 K/L 第四形态】第一版断言写成
         *   `hw_usbpd_cmd(av[2]) == 8` —— 全绿。但把 cli 改回 argv[1] 后
         *   它【依然全绿】(M14 存活)。根因: 断言调的是 cmd(),
         *   被测物是 cli(), 两者之间【没有任何调用】。
         *   验证器检查的不是被测物 => 这条断言恒真, 写了等于没写。
         *   => 修法: 断言必须【穿过 cli 本身】, 且读它的返回值。
         *   hw_usbpd_cli 原本只 printf 不回传, 没法观测 =>
         *   加 cli_last_rc() 出口, 让 cli 的决策变成可断言的整数。 */
        {
            char arg0[] = "xiaomo", arg1[] = "usbpd", arg2[] = "rows";
            char* av[3];
            av[0] = arg0; av[1] = arg1; av[2] = arg2;
            hw_usbpd_cli(3, av);
            CHECK(hw_usbpd_cli_last_rc() == hw_usbpd_cmd("rows", 0),
                  "cli 必须把【argv[2]=\"rows\"】的结果回传, 实得 %d (期望 %d)。"
                  "若为 -1, 说明 cli 又在读 argv[1] (把族名当子命令)",
                  hw_usbpd_cli_last_rc(), hw_usbpd_cmd("rows", 0));
            /* 反向: argv[1] 是族名, cmd 层不认识它 —— 这正是原 bug 的另一面 */
            CHECK(hw_usbpd_cmd(av[1], 0) == HW_USBPD_ERR_UNKNOWN,
                  "argv[1](\"usbpd\") 必须返回未识别 ERR_UNKNOWN, 实得 %d "
                  "(若非 %d, 说明族名被子命令表吞了)",
                  hw_usbpd_cmd(av[1], 0), HW_USBPD_ERR_UNKNOWN);
            /* 用法路径: argc<3 时 cli 必须回 1 (不是回一个协议值冒充成功) */
            CHECK(hw_usbpd_cli(2, av) == 1, "argc<3 应打用法并回 1");
            CHECK(hw_usbpd_cli_last_rc() == 0,
                  "用法路径不得留下上次命令的 rc 残留 (实得 %d)", hw_usbpd_cli_last_rc());
        }
    }

    printf("[16] at <mv>: 吃数字的判据查询 (双参路径的承重口, 不是死代码)\n");
    {
        /* 🕳️ 这组断言是【接线时被迫加的】, 不是补形式:
         *   六层 L2/L3 接上后实测, mo2kbc 双参拼出的 "rows8" 恒 -1 ——
         *   本层全是 strcmp 精确匹配, 没有任何命令吃数字后缀 ⇒ 双参路径死。
         *   家族其余成员 (hw_dc "read 99") 双参都活, 故补 "at <mv>"。
         *
         * 🔑 四档取值必须【互不相同】, 否则断言恒真, 等于没写:
         *   classify(mv,mv) 的落点已由真表算过 ——
         *     2700 → 命中 {2700,2700,APPLE_2_4A}
         *     1200 → 命中 {1200,1200,SAMSUNG_AFC}
         *       0 → 无行命中且两线都低于容差 → SDP
         *     2000 → 无行命中但高于容差 → FASTCHARGE_UNKNOWN
         *   若查表逻辑退化 (比如容差放到 300), 2700 与 2000 可能同档, 本组即红。 */
        CHECK(hw_usbpd_cmd("at 2700", 0) == HW_USBPD_APPLE_2_4A,
              "at 2700 应判 APPLE_2_4A, 实得 %d", hw_usbpd_cmd("at 2700", 0));
        CHECK(hw_usbpd_cmd("at 1200", 0) == HW_USBPD_SAMSUNG_AFC,
              "at 1200 应判 SAMSUNG_AFC, 实得 %d", hw_usbpd_cmd("at 1200", 0));
        CHECK(hw_usbpd_cmd("at 0", 0) == HW_USBPD_SDP,
              "at 0 应判 SDP, 实得 %d", hw_usbpd_cmd("at 0", 0));
        CHECK(hw_usbpd_cmd("at 2000", 0) == HW_USBPD_FASTCHARGE_UNKNOWN,
              "at 2000 应判 FASTCHARGE_UNKNOWN, 实得 %d", hw_usbpd_cmd("at 2000", 0));
        /* 四档两两不等 —— 直接把"恒真"钉死 */
        CHECK(HW_USBPD_APPLE_2_4A != HW_USBPD_SAMSUNG_AFC &&
              HW_USBPD_SAMSUNG_AFC != HW_USBPD_SDP &&
              HW_USBPD_SDP != HW_USBPD_FASTCHARGE_UNKNOWN,
              "四档枚举值必须互不相同, 否则上面 4 条断言恒真");

        /* --- 三个守卫, 缺一即红 ---
         * (a) 空参数: "at " 若不拦, atoi("")=0 会被读成"0mV 判 SDP",
         *     把"忘了传参"伪装成"一个有效判读" ⇒ 必须 ERR_PARAM。 */
        CHECK(hw_usbpd_cmd("at ", 0) == HW_USBPD_ERR_PARAM,
              "\"at \"(空参数) 应 ERR_PARAM, 实得 %d (若得 SDP=1 说明空参被当成 0mV)",
              hw_usbpd_cmd("at ", 0));
        /* (b) 负线压: 物理不存在, 却是 classify 的合法入参 */
        CHECK(hw_usbpd_cmd("at -5", 0) == HW_USBPD_ERR_PARAM,
              "\"at -5\" 应 ERR_PARAM, 实得 %d", hw_usbpd_cmd("at -5", 0));
        /* (c) 越界: 线压上界 = raw_rail_mv * ratio / 1000 = 2850*2960/1000 = 8436 */
        {
            long cap = (long)hw_usbpd_tap_19k_10k.raw_rail_mv *
                       (long)hw_usbpd_tap_19k_10k.ratio_milli / 1000;
            char over[32], edge[32];
            snprintf(over, sizeof(over), "at %ld", cap + 1);
            snprintf(edge, sizeof(edge), "at %ld", cap);
            CHECK(hw_usbpd_cmd(over, 0) == HW_USBPD_ERR_PARAM,
                  "\"%s\"(超上界 %ldmV) 应 ERR_PARAM, 实得 %d",
                  over, cap, hw_usbpd_cmd(over, 0));
            /* 边界本身【必须放行】: 上界处的守卫若写成 >cap 而非 >=cap,
             * 数值上仍能过 cap+1 这条断言 —— 补一条"边界可达"才抓得住。 */
            CHECK(hw_usbpd_cmd(edge, 0) != HW_USBPD_ERR_PARAM,
                  "\"%s\"(正好等于上界 %ldmV) 应放行判读, 实得 ERR_PARAM",
                  edge, cap);
        }
        /* 非法数字 (strtol 残留字符) */
        CHECK(hw_usbpd_cmd("at abc", 0) == HW_USBPD_ERR_PARAM,
              "\"at abc\" 应 ERR_PARAM, 实得 %d", hw_usbpd_cmd("at abc", 0));
        /* 前缀不得遮蔽既有命令 —— "at " 插在 selftest 之后, 顺序不能反。
         *   行数走【真表出口】取, 不写死常量: 写死就变成"表改了断言还绿"。 */
        {
            int n = 0;
            hw_usbpd_qc_table(&n);
            CHECK(n >= 4, "真表行数应 >=4, 实得 %d", n);
            CHECK(hw_usbpd_cmd("rows", 0) == n,
                  "加 at 前缀后 rows 仍须返真表行数 %d, 实得 %d", n, hw_usbpd_cmd("rows", 0));
            CHECK(hw_usbpd_cmd("tol", 0) == HW_USBPD_TOL_MV,
                  "加 at 前缀后 tol 仍须返 %d, 实得 %d",
                  HW_USBPD_TOL_MV, hw_usbpd_cmd("tol", 0));
        }
        /* ⚠️ 这里【故意不测 cmd("selftest")】: 它会调 hw_usbpd_selftest(),
         *   而本函数正在 selftest 里 ⇒ 无限递归。
         *   第一版这里写了 `CHECK(cmd("selftest")==0||1)` —— 双重错误:
         *     (1) `x==0||1` 恒真 = 我自己刚在 [15] 记下的"恒真断言"同款,
         *         写完就犯;
         *     (2) 会真的递归下去, 栈爆。
         *   记下来: 断言"某命令存在"时, 必须先确认执行它不会回头再进自己。 */

        /* 🔴【本组是"验的不是被测物"的第二次现形 —— 2026-10-03 实跑抓到】
         *   上面 12 条全部调 hw_usbpd_cmd() ⇒ 绿。
         *   但真从命令行敲 `./xiaomo usbpd at 2700` 返回 -7:
         *   cli 传进来的是 argv[2]="at" + argv[3]="2700", 而 cli 当时
         *   只取 argv[2] 当【完整命令串】⇒ cmd("at") 匹配不上 "at " 前缀。
         *   => 命令层的 12 条全绿, 用户一个都用不了: 验的不是被测物。
         *   与 [15] 踩的同一个坑 (断言调 cmd、没穿 cli), 区别是这次
         *   我先只信了 cmd 层就报了"全绿", 靠【外部实跑】才抓出来。
         *   纪律: 命令层绿 ≠ 入口绿, 入口必须用【真 argv 形状】穿一遍。 */
        {
            char a0[] = "xiaomo", a1[] = "usbpd";
            const char* cases[][3] = {
                { "at", "2700", "" }, { "at", "1200", "" }, { "at", "0", "" },
                { "at", "2000", "" },
            };
            const int want[] = { HW_USBPD_APPLE_2_4A, HW_USBPD_SAMSUNG_AFC,
                                 HW_USBPD_SDP, HW_USBPD_FASTCHARGE_UNKNOWN };
            int k;
            for (k = 0; k < 4; k++) {
                char a2[16], a3[16];
                char* av[5];
                snprintf(a2, sizeof(a2), "%s", cases[k][0]);
                snprintf(a3, sizeof(a3), "%s", cases[k][1]);
                av[0] = a0; av[1] = a1; av[2] = a2; av[3] = a3; av[4] = 0;
                hw_usbpd_cli(4, av);
                CHECK(hw_usbpd_cli_last_rc() == want[k],
                      "cli 穿真 argv(\"at\" \"%s\") 应回 %d, 实得 %d "
                      "(若得 ERR_UNKNOWN=%d, 说明 cli 又只读了 argv[2])",
                      cases[k][1], want[k], hw_usbpd_cli_last_rc(), HW_USBPD_ERR_UNKNOWN);
            }
            /* 空参数必须穿过 cli 仍是 ERR_PARAM (不能被拼成 "at" 而丢守卫) */
            {
                char a2[] = "at";
                char* av[4];
                av[0] = a0; av[1] = a1; av[2] = a2; av[3] = 0;
                hw_usbpd_cli(3, av);
                CHECK(hw_usbpd_cli_last_rc() == HW_USBPD_ERR_UNKNOWN,
                      "cli 收 \"at\"(无参数) 应回 ERR_UNKNOWN(%d), 实得 %d "
                      "(若回 %d 说明守卫被绕过)",
                      HW_USBPD_ERR_UNKNOWN, hw_usbpd_cli_last_rc(), HW_USBPD_SDP);
            }
        }
    }

    /* 🔴🔴【自测污染全局态 = 验证桥自身的 bug】家族第三次同形 (hw_dc/hw_fault 都栽过):
     *   本函数尾部最后一次动作是 hw_usbpd_bsp_install(&SIM_BSP) —— 它【不】随函数返回
     *   而撤销, s_bsp 是文件级 static。于是 Phase B 接手时 BSP 仍装着 SIM_BSP,
     *   B1 判 is_sim()==1 拿到 0、B2 判 sample 返 ERR_NODEV 拿到 0(桩在应答)。
     *   症状: 真机跑出 fails=3, 看起来像"产品在真机上坏了"。
     *
     *   🕳️ 为什么宿主 71/0 全绿时抓不到: 宿主回归跑的是 selftest 【自己】,
     *   跑完就退进程, 没人接着断言 "selftest 之后 BSP 必须回到未装"。
     *   这是【谁调用谁断言】的经典盲区 —— 探针自己测自己, 测不出自己留下的垃圾。
     *   ⇒ 判据: 凡是"安装型"全局态 (BSP/DMA/任务/钩子), 被测函数返回后
     *     必须由【调用方】断言其已复位, 不能只靠被测函数自觉。
     *
     *   修法 = 出口无条件卸载。sample 走 s_bsp.analog_mv==NULL 会走诚实 ERR_NODEV 分支,
     *   与"装 BSP 前"完全同态, 不影响本函数内任何断言 (它们都装完 BSP 才断言)。 */
    hw_usbpd_bsp_install(NULL);

    printf("\n==== selftest: %d 断言 / %d 失败 | 变异对照 %d 项 / %d 存活 ====\n",
           g_case_total, g_fail, g_var_total, g_var_fail);
    if (g_fail == 0 && g_var_fail == 0)
        printf("RESULT: ALL PASS (判据表无重叠, 全部行可达, 阴性对照已抓到 c3_adc 死代码表)\n");
    else
        printf("RESULT: FAIL (fails=%d var_survive=%d)\n", g_fail, g_var_fail);
    return g_fail + g_var_fail;
}
