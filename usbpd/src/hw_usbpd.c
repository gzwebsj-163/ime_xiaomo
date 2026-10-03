/* hw_usbpd.c —— USB-C 快充协议采集层 (xiaomo hw 家族第十二成员)
 *
 * 平台无关: 本文件不 include 任何 ESP-IDF / POSIX 头, 可直接编进宿主测试。
 * 真机 BSP 由调用方 (esp32s3_usbpd_test/) 提供并注入。
 */
#include "hw_usbpd.h"
#include <string.h>
#include <stdio.h>

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
        return HW_USBPD_ERR_PARAM;   /* 本次读数无意义, 不判读 */
    }

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
    /* 未接 FUSB302 = 无 PD 源。返回 ERR_UNSUPPORT 而非空列表假成功。 */
    if (!s_bsp.i2c_read) return HW_USBPD_ERR_UNSUPPORT;
    return HW_USBPD_ERR_UNSUPPORT;   /* 驱动待硬件到位后实现 */
}

/* ============================ 统计 ============================ */
void hw_usbpd_stat(hw_usbpd_stat_t* out) { if (out) *out = s_stat; }
void hw_usbpd_stat_reset(void) { memset(&s_stat, 0, sizeof(s_stat)); }
