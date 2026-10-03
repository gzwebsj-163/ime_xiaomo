/* hw_usbpd.h —— USB-C 快充协议采集层 (xiaomo hw 家族第十二成员)
 *
 * ============================================================================
 * ⚠️ 物理层事实 (不是注释里的装饰, 是接口设计的前提)
 * ----------------------------------------------------------------------------
 *   D+ / D-   跑: BC1.2 / QC2.0 / QC3.0 / Apple / 三星 AFC
 *   CC1 / CC2 跑: USB-PD 全部内容 (PDO/RDO 列表, 5/9/12/20V 协商)
 *
 *   PD 的每一个比特都在 CC 上, 物理上不经过 D+/-。
 *   => 只接 D+/D- 永远看不到 PD, 这不是灵敏度问题, 是接线问题。
 *   => 本模块把两条总线做成【两个独立通道】, 不假装一路线能读全。
 *
 *   依据: USB Type-C Cable and Connector Specification, CC 是 Biphase Mark
 *         编码的差分对 (300kbps); D+/D- 在 PD 期间仅承载 D+/D- 状态。
 * ============================================================================
 *
 * 家族对齐点 (对标 hw_pin / hw_dc / hw_dmc):
 *   1. SIM/REAL 隔离 —— BSP 函数指针表, 【部分可注入】
 *      任意回调可置 NULL => 该动作回落确定性模拟器
 *      (故可只注入 ADC, 其余通道仍可在宿主回归)
 *      分水岭判据: hw_usbpd_is_sim() 看关键指针是否为 NULL,
 *                  与 hw_pin.c:615 `use_dev = (g_bsp.gpio_read == NULL)` 同形。
 *   2. 自校验 —— hw_usbpd_selftest(), 含【构造性不变量】:
 *      判据表容差区间互不重叠 (直接挡住 c3_adc 的死代码表 bug)。
 *   3. 硬件坑落地 —— 悬空拾波闸 / 分压还原往返 / 通道缺失诚实失败,
 *      全部是【代码闸】而非注释。
 *
 * 协议判读 (D+/D- 还原后线电压, 典型值):
 *   0.75/0.25 = QC 5V  |  2.0/0.75 = QC 9V  |  2.0/1.5 = QC 12V(兼容档)
 *   2.7/2.0 = Apple 1A | 2.7/2.7 = Apple 2.4A | 1.2/1.2 = 三星 AFC
 *   注意: 这些是 D+/D- 【静态电平】, 而非 PD 报文 —— 两者不要混为一谈。
 */
#ifndef HW_USBPD_H
#define HW_USBPD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 通道定义 ============================ */
/* 物理通道: 两条【总线】, 每条 2 根线 */
typedef enum {
    HW_USBPD_CH_DP = 0,     /* D+  (BC/QC/Apple/AFC 总线) */
    HW_USBPD_CH_DM = 1,     /* D- */
    HW_USBPD_CH_CC1 = 2,    /* CC1 (PD 总线)  */
    HW_USBPD_CH_CC2 = 3,    /* CC2 */
    HW_USBPD_CH_MAX = 4
} hw_usbpd_ch_t;

/* ============================ BSP: 部分可注入 ============================
 * 约定:
 *   analog_mv  返回 0 成功并回填 mV; <0 = 该通道采集失败(诚实失败, 不回落)
 *   i2c_write  返回 0 成功 (FUSB302 寄存器写)
 *   i2c_read   返回 0 成功并回填 len 字节
 *   biphase_rx 返回捕获到的比特数; <0 失败
 *
 * 关键: analog_mv 置 NULL => 回落确定性模拟表 (SIM)
 *       analog_mv 非 NULL => REAL (分水岭)
 */
typedef struct {
    const char* name;
    /* 读 1 个模拟通道的毫伏值; ch ∈ [0, HW_USBPD_CH_MAX) */
    int (*analog_mv)(int ch, int* out_mv);
    /* FUSB302 I²C (PD 通道); 未接硬件时置 NULL */
    int (*i2c_write)(uint8_t reg, const uint8_t* buf, uint32_t len);
    int (*i2c_read)(uint8_t reg, uint8_t* buf, uint32_t len);
    /* CC 线上 Biphase 比特捕获 (PD 物理层); 未接硬件时置 NULL */
    int (*biphase_rx)(uint8_t* bits, uint32_t cap_bits);
} hw_usbpd_bsp_t;

/* 安装/卸载 BSP。卸载 = 传 NULL, 回落确定性模拟器。
 * ⚠️ 与 hw_dc 同一教训: init 类函数【不得】静默抹掉已装 BSP。 */
void hw_usbpd_bsp_install(const hw_usbpd_bsp_t* bsp);

/* 结构性分水岭 —— 与 hw_pin.c:615 同形, 用【单个指针】判, 不用缓存状态 */
int hw_usbpd_is_sim(void);   /* 1 = SIM, 0 = REAL */

/* ============================ 分压档案 (L2) ============================
 * 换分压电阻只改这张表, 判据代码零改动 (同 panel_lcd 范式)。 */
typedef struct {
    const char* name;
    uint32_t    ratio_milli;  /* 还原系数 ×1000: 线压 = TAP × ratio / 1000 */
    int         raw_rail_mv;   /* 超过此值 = 悬空拾波/未接线, 闸门不判读 */
    int         quiet_mv;      /* 低于此 = 该线确认为低(非未接线) */
} hw_usbpd_tap_t;

extern const hw_usbpd_tap_t hw_usbpd_tap_19k_10k;   /* 19.6k/10k → 2960 */

/* ============================ 判据 (L2) ============================ */
typedef enum {
    HW_USBPD_UNKNOWN = 0,
    HW_USBPD_SDP,             /* 标准下行端口, 无快充 */
    HW_USBPD_DCP_BC12,        /* 专用充电端口 */
    HW_USBPD_DCP_SHORT,       /* D+/D- 短接 */
    HW_USBPD_QC_5V,
    HW_USBPD_QC_9V,
    HW_USBPD_QC_12V,
    HW_USBPD_APPLE_1A,
    HW_USBPD_APPLE_2A,
    HW_USBPD_APPLE_2_4A,
    HW_USBPD_APPLE_1_5A,
    HW_USBPD_SAMSUNG_AFC,
    HW_USBPD_FASTCHARGE_UNKNOWN /* 有分压但落在已知表外 */
} hw_usbpd_verdict_t;

const char* hw_usbpd_verdict_name(hw_usbpd_verdict_t v);

typedef struct {
    hw_usbpd_verdict_t verdict;
    int   dp_mv, dm_mv;       /* 还原后的线电压 */
    int   dp_raw, dm_raw;     /* TAP 原始读数 */
    int   valid;              /* 0 = 被悬空闸拦下, 本次读数无意义 */
} hw_usbpd_dp_result_t;

/* 判读容差 mV。取 50 而非更宽:
 *   1% 电阻 + 12bit ADC, 端到端误差约 ±30mV, 故 50mV 是"能分辨且不误判"的量级。
 *   ⚠️ 不能再宽: BC1.2 的 600mV 与 QC5V 的 750mV 物理上只差 150mV,
 *      容差一旦 >75mV 这两行判读方框必重叠 => 后者变死代码。
 *      (c3_adc 版正是栽在这里, 容差 300mV)
 */
#define HW_USBPD_TOL_MV 50

/* ================== 判据表【只读导出】==================
 * 🕳️ 为什么必须导出真表给检查器 (变异测试 M2 实锤, 2026-10-03):
 *   原先 selftest 里的 table_invariant_check() 手抄了一份 T[], 检查的是
 *   【副本】。变异把真表 QC_TBL 塞回 c3_adc 的死代码行 {500,0} 时, 真表已经
 *   出现重叠(与 {600,0} 间距 100mV = 2*TOL), 但检查器遍历副本看不到 => 存活。
 *   更糟的是注释里承诺的 "[12] 双份失配探针" 从来没写过 ([1]~[11] 就没有 12),
 *   = 用一句注释冒充了一道守卫。
 *   => 结论: 不变量检查器【必须遍历被测物本身】, 不允许第二份副本。
 *      本 accessor 是唯一真表出口, 任何检查器都走它。 */
typedef struct {
    int dp, dm;                 /* D+/D- 线电压 mV (方框中心) */
    hw_usbpd_verdict_t v;
} hw_usbpd_qc_row_t;

/* 返回真判据表首地址与行数; *n_rows 非 NULL 时回填。永不返回 NULL。 */
const hw_usbpd_qc_row_t* hw_usbpd_qc_table(int* n_rows);

/* ============================ 状态快照 ============================ */
typedef struct {
    uint32_t samples;
    uint32_t dp_rail_rejects;  /* 悬空闸命中次数 (硬件坑计数) */
    uint32_t ch_fail;          /* BSP 返回 <0 的诚实失败次数 */
    uint32_t verdicts_changed; /* 判据变化次数 */
    uint32_t i2c_err;          /* FUSB302 读失败 */
    hw_usbpd_verdict_t last;
} hw_usbpd_stat_t;

/* ============================ API ============================ */
void hw_usbpd_init(const hw_usbpd_tap_t* tap);
void hw_usbpd_set_tap(const hw_usbpd_tap_t* tap);   /* 热换档案 */
const hw_usbpd_tap_t* hw_usbpd_get_tap(void);

/* 采样一拍: 读 D+/D- → 悬空闸 → 还原 → 查表。填 result 返回。 */
int hw_usbpd_sample(hw_usbpd_dp_result_t* result);

/* 从已还原的线电压直接查表 (纯函数, 宿主可测) */
hw_usbpd_verdict_t hw_usbpd_classify(int dp_mv, int dm_mv);

/* PD 通道 (FUSB302B)。⚠️ 未接硬件时诚实返回错误码, 不返回假数据。 */
#define HW_USBPD_OK              0
#define HW_USBPD_ERR_PARAM      -1
#define HW_USBPD_ERR_NODEV      -2   /* BSP 未装该动作 → 回落模拟 */
#define HW_USBPD_ERR_HW         -3   /* 真机 I²C 失败 */
#define HW_USBPD_ERR_UNSUPPORT  -4   /* 该协议本层不实现 (走 CC/PD 栈) */

int  hw_usbpd_pd_poll(hw_usbpd_stat_t* stat);   /* 读 FUSB302 状态寄存器 */
int  hw_usbpd_pd_read_pdo(uint32_t* pdo_list, int max, int* n_out);

void hw_usbpd_stat(hw_usbpd_stat_t* out);
void hw_usbpd_stat_reset(void);

/* ============================ 自校验 ============================ */
/* 返回 fails 计数 (0 = 全过)。覆盖: 表构造性不变量 / 还原往返 /
 * 悬空闸 / 分水岭 / 死代码表防护 / 诚实失败路径。 */
int hw_usbpd_selftest(void);

/* 黄金值: 只覆盖【可跨口径复现的标量字段】(ratio/rail/quiet)。
 *
 * 🕳️ 曾经的坑: 一开始哈希整个 struct (含 name 指针), 那个值每次编译/运行
 *    都可能变 (指针受 ASLR/链接布局影响) => 黄金值不可复现 = 没有黄金值。
 *    与 hw_flash/hw_dc 家族同源问题: 指纹必须钉在【语义量】上, 不是内存布局上。
 * 改判据表或改分压档案必然改此值; 换编译器不该改。 */
#define HW_USBPD_GOLDEN 0xC481F6E5u

#ifdef __cplusplus
}
#endif
#endif /* HW_USBPD_H */
