/* hw_usbpd.c —— USB-C 快充协议采集层 (xiaomo hw 家族第十二成员)
 *
 * 平台无关: 本文件不 include 任何 ESP-IDF / POSIX 头, 可直接编进宿主测试。
 * 真机 BSP 由调用方 (esp32s3_usbpd_test/) 提供并注入。
 */
#include "hw_usbpd.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* strtol —— 与 hw_dc.c:657 同款用法 */

/* ============================ 分压档案 ============================ */
/* 19.6k 上臂 / 10k 下臂: TAP = 线压 × 10 / 29.6 => ratio_milli = 2960 */
const hw_usbpd_tap_t hw_usbpd_tap_19k_10k = {
    "19k/10k", 2960u,
    /* raw_rail_mv: TAP 脚电压 3.3V 轨的 86.5% —— 悬空拾波会顶到接近 VDD */
    2850,
    /* quiet_mv: 低于 30mV 视为确认为低 (TAP 自身底噪量级) */
    30
};

static const hw_usbpd_tap_t* s_tap = &hw_usbpd_tap_19k_10k;
static hw_usbpd_bsp_t s_bsp;
static hw_usbpd_stat_t s_stat;

/* ============================ BSP 安装 ============================ */
void hw_usbpd_bsp_install(const hw_usbpd_bsp_t* bsp)
{
    /* ⚠️ 与 hw_dc 同一教训: 此函数【不】调 init, 不抹状态。
     * 卸载 = 传 NULL => 结构体归零 => hw_usbpd_is_sim() 回到 1。 */
    if (bsp) s_bsp = *bsp;
    else    memset(&s_bsp, 0, sizeof(s_bsp));
}

/* 结构性分水岭: 与 hw_pin.c:615 `use_dev = (g_bsp.gpio_read == NULL)` 同形。
 * 【不用】缓存的环境标志 —— 那样安装后忘改标志就会静默跑错模式。 */
int hw_usbpd_is_sim(void)
{
    return (s_bsp.analog_mv == NULL) ? 1 : 0;
}

/* ============================ 档案 ============================ */
void hw_usbpd_init(const hw_usbpd_tap_t* tap)
{
    if (tap) s_tap = tap;
    memset(&s_stat, 0, sizeof(s_stat));
}
void hw_usbpd_set_tap(const hw_usbpd_tap_t* tap) { if (tap) s_tap = tap; }
const hw_usbpd_tap_t* hw_usbpd_get_tap(void) { return s_tap; }

/* ============================ 判据表 ============================
 * 还原后线电压判据表 (L2)。
 *
 * 【构造性不重叠】: 任意两行的判读方框 [±TOL] 必须互不相交,
 * 即两行间距 > 2*TOL = 100mV (至少一个坐标)。selftest 逐对验证。
 *
 * 这直接挡住 c3_adc 版 proto_tap.c:30-31 的死代码表:
 *   ( {600,0,"BC1.2"} 与 {500,0,"DCP"} 相差 100mV < 2*300mV, 后者永不可达 )。
 *
 * ⚠️ 物理事实: BC1.2 的 D+≈600mV 与 QC5V 的 D+≈750mV 只差 150mV,
 *    这就是容差必须收紧到 50mV 的原因 —— 放宽必然吃掉一行。
 *
 * SDP(标准下行端口)不进表: D+/D- 双低, 由 classify() 的底噪闸单独判定,
 * 因为它与 BC1.2 在【语义】上就不是一类 (一个不提供充电, 一个提供)。
 */
static const hw_usbpd_qc_row_t QC_TBL[] = {
    /* --- QC2.0: D+ 高 / D- 低, 靠 D- 电平分档 --- */
    {  750,  250, HW_USBPD_QC_5V  },   /* QC 5V 基础档 */
    { 2000,  750, HW_USBPD_QC_9V  },   /* QC 9V  */
    { 2000, 1500, HW_USBPD_QC_12V },   /* QC 12V (dm 与上行距 750) */
    /* --- Apple: D+ 分档, D- 也在 2.0~2.7V 区间 --- */
    { 2700, 2700, HW_USBPD_APPLE_2_4A },
    { 2700, 2000, HW_USBPD_APPLE_1A   },   /* 同 dp 2700, dm 差 700 */
    { 2000, 2700, HW_USBPD_APPLE_1_5A },   /* 同 dm 2700, dp 差 700 */
    /* --- 其它 --- */
    { 1200, 1200, HW_USBPD_SAMSUNG_AFC},
    {  600,    0, HW_USBPD_DCP_BC12  },   /* D+ 600mV/D- 0 = 专用充电端口 */
};
#define QC_N ((int)(sizeof(QC_TBL) / sizeof(QC_TBL[0])))

/* 真表只读出口 —— 不变量检查器必须走这里, 不许手抄副本 (见头文件 M2 复盘) */
const hw_usbpd_qc_row_t* hw_usbpd_qc_table(int* n_rows)
{
    if (n_rows) *n_rows = QC_N;
    return QC_TBL;
}

const char* hw_usbpd_verdict_name(hw_usbpd_verdict_t v)
{
    switch (v) {
    case HW_USBPD_SDP:              return "SDP(标准端口)";
    case HW_USBPD_DCP_BC12:         return "BC1.2/DCP";
    case HW_USBPD_DCP_SHORT:        return "DCP(短接)";
    case HW_USBPD_QC_5V:            return "QC2.0 5V";
    case HW_USBPD_QC_9V:            return "QC2.0 9V";
    case HW_USBPD_QC_12V:           return "QC2.0 12V";
    case HW_USBPD_APPLE_1A:         return "Apple 1A";
    case HW_USBPD_APPLE_2A:         return "Apple 2A";
    case HW_USBPD_APPLE_1_5A:       return "Apple 1.5A";
    case HW_USBPD_APPLE_2_4A:       return "Apple 2.4A";
    case HW_USBPD_SAMSUNG_AFC:      return "三星AFC";
    case HW_USBPD_FASTCHARGE_UNKNOWN: return "有分压/未识别";
    default:                        return "未识别";
    }
}

/* 纯函数查表 —— 宿主可直接测 */
hw_usbpd_verdict_t hw_usbpd_classify(int dp_mv, int dm_mv)
{
    int i;
    for (i = 0; i < QC_N; i++) {
        int a = dp_mv - QC_TBL[i].dp;
        int b = dm_mv - QC_TBL[i].dm;
        if (a <= HW_USBPD_TOL_MV && -a <= HW_USBPD_TOL_MV &&
            b <= HW_USBPD_TOL_MV && -b <= HW_USBPD_TOL_MV)
            return QC_TBL[i].v;
    }
    /* 有任一线明显高于底噪但都不匹配 = 有分压但非已知协议 */
    if (dp_mv > HW_USBPD_TOL_MV || dm_mv > HW_USBPD_TOL_MV)
        return HW_USBPD_FASTCHARGE_UNKNOWN;
    return HW_USBPD_SDP;
}

/* ============================ 采样 ============================ */
int hw_usbpd_sample(hw_usbpd_dp_result_t* result)
{
    int dp_raw = 0, dm_raw = 0;
    int rc_dp, rc_dm;

    if (!result) return HW_USBPD_ERR_PARAM;
    memset(result, 0, sizeof(*result));

    s_stat.samples++;

    /* --- BSP 诚实失败: 通道读不出来就报出来, 不拿 0 冒充 --- */
    if (hw_usbpd_is_sim()) {
        /* 未装 BSP => 本层无数据源。返回 ERR_NODEV 而非 ERR_HW:
         * 调用方必须能分清「没装 BSP」(代码侧问题) 与
         * 「装了但硬件读失败」(设备侧问题) —— 否则排查时会被误导。 */
        rc_dp = HW_USBPD_ERR_NODEV;
        rc_dm = HW_USBPD_ERR_NODEV;
    } else {
        rc_dp = s_bsp.analog_mv(HW_USBPD_CH_DP, &dp_raw);
        rc_dm = s_bsp.analog_mv(HW_USBPD_CH_DM, &dm_raw);
    }
    if (rc_dp < 0 || rc_dm < 0) {
        s_stat.ch_fail++;
        /* 传播底层码, 不统一压成 ERR_HW (见上)。 */
        if (rc_dp == HW_USBPD_ERR_NODEV || rc_dm == HW_USBPD_ERR_NODEV)
            return HW_USBPD_ERR_NODEV;
        return HW_USBPD_ERR_HW;
    }

    result->dp_raw = dp_raw;
    result->dm_raw = dm_raw;

    /* --- 硬件坑① 悬空拾波闸: 未接线时 TAP 会被耦合到 VDD 轨附近 --- */
    if (dp_raw > s_tap->raw_rail_mv || dm_raw > s_tap->raw_rail_mv) {
        s_stat.dp_rail_rejects++;
        result->valid = 0;
        /* 🕳️ 专用码 (2026-10-03): 原为 ERR_PARAM —— 调用方参数完全正确,
         *   返回 PARAM 会把排查者引去检查自己的指针, 失败原因指错方向。 */
        return HW_USBPD_ERR_FLOAT;
    }

    /* --- 硬件坑③ 底噪确认闸 (2026-10-03 新增, 兑现 quiet_mv 的语义) ---
     *
     * 🕳️【假守卫复盘】quiet_mv 此前是【死字段】: 只在头文件定义、初始化、
     *   并被黄金值哈希, 采样路径【从不读它】。受控实验把 30 改成 9999
     *   (噪声门放宽 300 倍), 30 条断言里【只有黄金值那条红】, 行为断言全过
     *   ⇒ 判据层完全无感。
     *   这比死字段更坏: 黄金值让人误以为"底噪阈值被锁住了"。
     *   (锚点: 恒真/恒零信号比缺失信号更险, 因为它让你有依据地放心。)
     *
     * 【真语义】TAP 板自身有底噪, 未接线的线也可能读出几十 mV 的感应值。
     *   若不确认"这根线真的接了", 一根悬空的 D- 会被当成低电平,
     *   走进 BC1.2 判据 (D+ 600mV / D- 0) => 把没接的线判成 BC1.2 假成功。
     *
     * 🔑【域必须对】闸门判定在【原始域 (TAP 脚/ADC 输入)】, 不是还原后的线电压域。
     *   这不是随手选的: quiet_mv 与 rail_mv 描述的是同一个物理点 —— TAP 引脚
     *   上的电压 (rail_mv 的注释就写着 "TAP 脚电压 3.3V 轨的 86.5%")。
     *   把两个闸门放在不同域, 换分压电阻时它们会各自漂移, 失去可比性。
     *   🕳️ 这一点是被 [14] 的断言抓出来的, 不是想出来的: 我第一版把闸门
     *   放在还原域, 结果"灌 40mV" 经整数除法往返后还原成 38mV, 恰好越过
     *   quiet_mv=30 ⇒ 闸门不触发。断言红, 顺着量纲查下去才发现域错了。
     *
     * 【为何不判废整次采样】DCP_BC12 的判据就是 dm=0。若把 dm 低于底噪
     *   判为废, BC1.2 这档会被自己杀死。正确做法是【归一为 0 再判读】,
     *   而不是拒绝 —— 底噪与真 0 在判据层面是等价的, 区别只在于可信度。
     */
    if (dp_raw >= 0 && dp_raw < s_tap->quiet_mv) dp_raw = 0;   /* 底噪 → 确认接地 */
    if (dm_raw >= 0 && dm_raw < s_tap->quiet_mv) dm_raw = 0;
    result->dp_raw = dp_raw;
    result->dm_raw = dm_raw;

    /* --- 还原: 线压 = TAP × ratio / 1000 --- */
    result->dp_mv = (int)((int32_t)dp_raw * (int32_t)s_tap->ratio_milli / 1000);
    result->dm_mv = (int)((int32_t)dm_raw * (int32_t)s_tap->ratio_milli / 1000);

    result->verdict = hw_usbpd_classify(result->dp_mv, result->dm_mv);
    result->valid = 1;

    if (result->verdict != s_stat.last) {
        s_stat.verdicts_changed++;
        s_stat.last = result->verdict;
    }
    return HW_USBPD_OK;
}

/* ============================ PD 通道 (FUSB302B) ============================
 * ⚠️ 物理层事实: PD 在 CC 上, D+/D- 读不到 PDO/RDO。
 *    本函数走 FUSB302 寄存器, 读不到就诚实返回错误, 【绝不】编造 PDO。
 */
int hw_usbpd_pd_poll(hw_usbpd_stat_t* stat)
{
    uint8_t buf[8];
    if (stat) memset(stat, 0, sizeof(*stat));
    if (!s_bsp.i2c_read) {
        /* BSP 未装 I²C = 回落模拟 = 本层不提供 PD 数据 */
        return HW_USBPD_ERR_UNSUPPORT;
    }
    /* FUSB302B STATUS 寄存器 (0x04) */
    if (s_bsp.i2c_read(0x04, buf, 2) < 0) {
        s_stat.i2c_err++;
        return HW_USBPD_ERR_HW;
    }
    if (stat) { stat->i2c_err = s_stat.i2c_err; }
    return HW_USBPD_OK;
}

int hw_usbpd_pd_read_pdo(uint32_t* pdo_list, int max, int* n_out)
{
    if (!pdo_list || max <= 0 || !n_out) return HW_USBPD_ERR_PARAM;
    *n_out = 0;
    /* 🕳️ 原为 `if (!s_bsp.i2c_read) return ERR_UNSUPPORT; return ERR_UNSUPPORT;`
     *   —— 两个分支同码, 装饰性 if, 读起来像"接了 FUSB302 就能读"但其实不能。
     *   (锚点: 恒真信号比缺失信号更险 —— 它让人以为这条路已经通了。)
     *   改为单条诚实路径: PDO 解码属于第③期范围, 现在【一律】诚实拒绝。 */
    (void)s_bsp.i2c_read;   /* 显式标注: 未来实现点就在这里 */
    return HW_USBPD_ERR_UNSUPPORT;   /* PDO 解码未实现 (第③期) —— 绝不编造 PDO */
}

/* ============================ 统计 ============================ */
void hw_usbpd_stat(hw_usbpd_stat_t* out) { if (out) *out = s_stat; }
void hw_usbpd_stat_reset(void) { memset(&s_stat, 0, sizeof(s_stat)); }

/* ============================ 六层 L4: 命令入口 ============================
 * 与 hw_dmc_cmd()/hw_dc_cmd() 同形: 一条命令串进, 一个整数出。
 * 【设计原则】每个命令的返回值必须【自身可证伪】:
 *   返回协议枚举值 (非 0) 时, 调用方能直接区分是哪一档, 不必再回查日志。
 *   恒 0/1 的布尔量不算合格出口 —— 那是"看起来接上了"的假接线 (锚点 H/F)。
 */
int hw_usbpd_cmd(const char* cmd, void* ctx)
{
    hw_usbpd_dp_result_t r;
    hw_usbpd_stat_t st;
    int rc;
    (void)ctx;
    if (!cmd) return HW_USBPD_ERR_PARAM;

    if (strcmp(cmd, "sim") == 0)        return hw_usbpd_is_sim();
    if (strcmp(cmd, "table") == 0)      return hw_usbpd_qc_table(0) ? QC_N : 0;
    if (strcmp(cmd, "rows") == 0)       return QC_N;
    if (strcmp(cmd, "tol") == 0)        return HW_USBPD_TOL_MV;
    if (strcmp(cmd, "golden") == 0)     return (int)HW_USBPD_GOLDEN;
    if (strcmp(cmd, "verdicts") == 0)   return (int)HW_USBPD_FASTCHARGE_UNKNOWN; /* 枚举上界 */
    if (strcmp(cmd, "tapname") == 0)    return s_tap->name ? 1 : 0;
    if (strcmp(cmd, "ratio") == 0)      return (int)s_tap->ratio_milli;
    if (strcmp(cmd, "quiet") == 0)      return s_tap->quiet_mv;
    if (strcmp(cmd, "rail") == 0)       return s_tap->raw_rail_mv;

    /* sample: 返回判定枚举 (0=UNKNOWN, >0 = 具体协议档) —— 调用方可自证 */
    if (strcmp(cmd, "sample") == 0) {
        rc = hw_usbpd_sample(&r);
        if (rc != HW_USBPD_OK) return rc;      /* 负码直通, 含 ERR_FLOAT */
        return (int)r.verdict;
    }
    /* dp/dm: 最近一次采样的还原线电压 (需先 sample) */
    if (strcmp(cmd, "dp") == 0 || strcmp(cmd, "dm") == 0) {
        rc = hw_usbpd_sample(&r);
        if (rc != HW_USBPD_OK) return rc;
        return (cmd[1] == 'p') ? r.dp_mv : r.dm_mv;
    }
    if (strcmp(cmd, "valid") == 0) {
        rc = hw_usbpd_sample(&r);
        return (rc == HW_USBPD_OK) ? r.valid : 0;
    }
    /* 硬件坑计数 (可证伪的整数, 不是布尔) */
    if (strcmp(cmd, "rail_hits") == 0)  { hw_usbpd_stat(&st); return (int)st.dp_rail_rejects; }
    if (strcmp(cmd, "ch_fail") == 0)    { hw_usbpd_stat(&st); return (int)st.ch_fail; }
    if (strcmp(cmd, "samples") == 0)    { hw_usbpd_stat(&st); return (int)st.samples; }
    if (strcmp(cmd, "changed") == 0)    { hw_usbpd_stat(&st); return (int)st.verdicts_changed; }
    if (strcmp(cmd, "last") == 0)       { hw_usbpd_stat(&st); return (int)st.last; }
    if (strcmp(cmd, "pdpoll") == 0)     { return hw_usbpd_pd_poll(&st); }
    if (strcmp(cmd, "selftest") == 0)   { return hw_usbpd_selftest(); }

    /* at <mv>: 无硬件也能问判据 —— 查【已还原线压相同】这一物理上常见的档
     *   (Apple 2.4A=2.7/2.7, 三星 AFC=1.2/1.2, BC1.2 空档=0/0)。
     *   🕳️ 为什么必须有这条 (2026-10-03 接线时实测): 本层全部命令都是
     *   strcmp 精确匹配, 而【没有任何命令吃数字后缀】⇒ mo2kbc 双参
     *   (hw_usbpd("fmt", 数值) → imm=R_TMP+1) 拼出的 "rows8" 恒 -1,
     *   整条双参路径结构性死掉。家族其余成员 (hw_dc "read 99") 双参都是活的,
     *   留一条死路径进六层 = 把死代码固化。
     *   ⚠️ 三个守卫, 且【必须在查表之前】:
     *     (a) 空参数/非法数字 —— 靠 strtol 的 end 指针判定:
     *         end == p  ⇔ 一个数字都没解析出来 (含空串 "at ") ⇔ ERR_PARAM
     *         *end != 0 ⇔ 有残留字符 ("at abc"/"at 12x")   ⇔ ERR_PARAM
     *         🕳️ 这里原先还另写了一句 `if (!*p) return ERR_PARAM;` 专门拦空串,
     *           变异 M11 拆掉它 → selftest【全绿】⇒ 证明那句是冗余死代码
     *           (strtol 的 end==p 已经兜住)。按"没变异杀死的守卫就是没被依赖"
     *           的纪律已删, 只留注释说明真实机制是什么。
     *     (b) 负线压            → 物理上不存在, 却是 classify 的合法入参。
     *     (c) 上界              → 分压档案 raw_rail_mv 是 TAP 侧上限,
     *                            线压上限 = rail*ratio/1000, 超界读数无意义。 */
    if (strncmp(cmd, "at ", 3) == 0) {
        const char* p = cmd + 3;
        char* end = 0;
        long mv;
        mv = strtol(p, &end, 10);
        if (end == p || *end != '\0') return HW_USBPD_ERR_PARAM;  /* (a) 空/非法 */
        if (mv < 0) return HW_USBPD_ERR_PARAM;              /* (b) 负线压 */
        if (mv > (long)s_tap->raw_rail_mv * (long)s_tap->ratio_milli / 1000)
            return HW_USBPD_ERR_PARAM;                      /* (c) 越界 */
        return (int)hw_usbpd_classify((int)mv, (int)mv);
    }

    if (strcmp(cmd, "help") == 0) return HW_USBPD_HELP;   /* 独立码位, 不与 ERR_NODEV 抢 -2 */
    return HW_USBPD_ERR_UNKNOWN;                         /* 独立码位, 不与 ERR_PARAM 抢 -1 */
}

/* 🕳️【可观测出口, 2026-10-03】cli 原先只 printf 不回传决策, 导致
 *   "cli 到底把哪个 argv 元素当子命令" 这件事【无法被断言】——
 *   selftest 只能验 cmd(), 于是写出一条恒真的假断言 (M14 存活)。
 *   本出口让 cli 的决策变成可断言的整数: 断言穿过 cli 本身,
 *   而不是绕过它去验底层 (锚点 K: 检查器必须检查被测物本身)。 */
static int s_cli_rc;
int hw_usbpd_cli_last_rc(void) { return s_cli_rc; }

int hw_usbpd_cli(int argc, char** argv)
{
    /* 🕳️【契约对齐, 2026-10-03 修】本函数原先取 argv[1] 当子命令, 但家族
     *   约定是 main.c 原样传 (argc, argv), 子命令在 argv[2]
     *   (对照 hw_dmc_cli: `if (argc < 3)` + `strcmp(argv[2], ...)`)。
     *   写成 argv[1] 时, `./xiaomo usbpd sim` 实际把 "usbpd" 当成了子命令,
     *   于是每个命令都落进"未识别"返回 -1 —— 看起来像"整条命令线没接上",
     *   真因却只是偏移 1。
     *   症状与病因分属两层时, 查"另一层"比反复改本层快得多。 */
    if (argc < 3) {
        printf("usage: xiaomo usbpd <cmd>\n");
        printf("  sim         1=SIM(未装BSP) 0=REAL\n");
        printf("  sample      采样判读 → 返回【协议判定枚举】(负码=诚实失败)\n");
        printf("  dp/dm       最近一次采样的还原线电压 (mV)\n");
        printf("  valid       本次读数是否有效\n");
        printf("  rows/tol    判据表行数 / 判读容差 (mV)\n");
        printf("  golden      黄金指纹\n");
        printf("  ratio/quiet/rail  当前分压档案三参数\n");
        printf("  rail_hits   悬空闸命中次数 (硬件坑①)\n");
        printf("  ch_fail     通道诚实失败次数 (硬件坑②)\n");
        printf("  samples/changed/last  采样总数/判定变化数/上次判定\n");
        printf("  pdpoll      读 FUSB302 状态 (无硬件→诚实失败)\n");
        printf("  at <mv>     无硬件查判据: classify(mv,mv) → 协议枚举 (吃数字, 双参入口)\n");
        printf("  selftest    全量自检\n");
        printf("  help        本用法\n");
        s_cli_rc = 0;          /* 用法不是一次判读, 不留上次 rc 残留 */
        return 1;
    }
    {
        /* 🕳️【多词命令拼接, 2026-10-03 修】本层原先只取 argv[2] 当【完整】
         *   命令串, 于是任何带参数的命令 (如 "at 2700") 都不可达:
         *   `./xiaomo usbpd at 2700` 传进来的 argv[2]="at", argv[3]="2700",
         *   只读 argv[2] ⇒ cmd("at") 匹配不上 "at " 前缀 ⇒ 落 ERR_UNKNOWN。
         *   症状是"命令明明在 help 里, 打出来却永远是 -7"。
         *
         *   ⚠️ 家族里两套约定并存, 别混:
         *     · VM/.mo 路径: 格式串【自带尾空格】, VM 拼数字
         *       (dmc_test.mo: `hw_dmc("add ", v100)` → "add 100")
         *     · CLI 路径: hw_dc_cli 是自己 strtol(argv[3]) 逐个读的,
         *       同样【不拼命令串】—— 那种写法只对 strcmp 分派的参数成立。
         *   本层把参数统一喂给【同一个 cmd 分派器】(而非在 cli 里另写一套
         *   strtol 分支), 所以必须在这里把 argv[3..] 用空格拼回去,
         *   让 cli 与 .mo 两条路径走【完全相同】的判读代码。
         *   => 一处判读逻辑, 两条入口, 不会再出现"两个出口两种行为"。 */
        char joined[256];
        int i, n = 0;
        joined[0] = '\0';
        for (i = 2; i < argc; i++) {
            size_t need = strlen(argv[i]) + 1;
            if (n + (int)need + 1 >= (int)sizeof(joined)) break;  /* 截断即止, 不越界 */
            if (n > 0) joined[n++] = ' ';
            memcpy(joined + n, argv[i], strlen(argv[i]));
            n += (int)strlen(argv[i]);
            joined[n] = '\0';
        }
        {
            int rc = hw_usbpd_cmd(joined, 0);
            if (rc == HW_USBPD_HELP) { hw_usbpd_cli(1, argv); return 1; }
            s_cli_rc = rc;
            printf("%d\n", rc);
            /* 🔴【退出码语义, 2026-10-03 修 —— 家族约定对齐】
             * 本行原先恒 `return 0`, 于是【自检红了进程也报成功】。
             * 后果不是"不好看", 而是【假的绿通道】: 任何靠退出码判成败的
             * 脚本/CI 都看不见 selftest 失败 —— 变异测试 15 项曾因此
             * 从"15 杀 0 存活"掉到"1 杀 14 存活", 而 14 个"存活"里有
             * 绝大多数根本没被真正断言, 只有一个是靠崩溃撞出来的。
             *
             *   家族约定(dmc): `if (strcmp(argv[2],"selftest")==0)
             *                     return hw_dmc_selftest(puts);` —— 直通。
             *
             * ⚠️ 这里【不是】第二个真相源: 命令是否存在仍由 cmd() 唯一判定
             *   (rc 来自它), 本分支只决定"这个数字怎么交给操作系统"。
             *   同理 selftest 内部的用例也会调 hw_usbpd_cli(3, "at"),
             *   但命令名不是 selftest ⇒ 不会自我递归。 */
            if (strcmp(joined, "selftest") == 0) return rc;   /* 失败计数直通 */
            return 0;                                         /* 其余是判读值, 打印即可 */
        }
    }
}
