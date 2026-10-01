/*
 * xiaomo - DC 电源信号层 (hw_dc, 2026-10-01)
 *
 * 全跨式设计 (与 hw_token/hw_fault/hw_core 家族同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_DC_MODE_OVERRIDE=n 强指 (KELL 内核内
 *      __linux__ 亦被定义, 故 KELL 宏优先级必须高于 linux)。
 *   2. 核心 (模式/信号表/组合数据帧/极值判决/校验和) 仅依赖
 *      stdint+string, 真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 信号采集走 BSP 回调注入:
 *        默认 = 确定性模拟 (表驱动, 全平台逐位一致可回归);
 *        真机固件 hw_dc_bsp_install() 注入真实 ADC/GPIO 读取, 判决逻辑零改动。
 *   4. 黄金参考 HW_DC_GOLDEN = (信号表 name+value) + (数据表 4B/项)
 *      FNV-1a-32 (独立 Python 对拍锁定, 2026-10-01), selftest 逐项断言。
 *
 * 六层接入 (2026-10-01):
 *   Makefile   HW_SRCS 收编 (macOS + Makefile.linux)
 *   VM 内核    OP_HW_DC_CALL (vm_core.c, kvm_run 上电自动 hw_dc_init)
 *   编译器     mo2kbc 内置 hw_dc("...") / hw_dc("fmt", 数值)
 *   CLI        ./xiaomo dc [card|sig N|set N V|base N|range N|data N|mode|selftest]
 *   示例       examples/dc_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh dc 块
 */
#include "hw_dc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 黄金校验和: 独立 Python 对拍锁定 (2026-10-01), 勿改 */
#define HW_DC_GOLDEN 0x028EECE6u

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t dc_mode_probe(void)
{
#if defined(HW_DC_MODE_OVERRIDE)
    return (uint8_t)HW_DC_MODE_OVERRIDE;
#elif defined(HW_DC_KELL)
    return (uint8_t)HW_DC_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_DC_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)   || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_DC_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_DC_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_DC_MODE_HOST;
#else
    return (uint8_t)HW_DC_MODE_TEST;
#endif
}

uint8_t hw_dc_mode(void)
{
    static uint8_t cached = 0xFFu;   /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = dc_mode_probe();
    return cached;
}

static const char* const dc_mode_names[HW_DC_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_dc_mode_str(uint8_t mode)
{
    return (mode < HW_DC_MODE_MAX) ? dc_mode_names[mode] : "?";
}

/* ============================================================
 * 2. DC 信号表 (用户原式默认值, 黄金校验和保护)
 *    索引 = dc_signal_t (DC_INPUT..DC_LOW_LITE, 共 8)
 * ============================================================ */
typedef struct {
    const char* name;    /* 短名 (黄金折叠对象) */
    const char* desc;    /* 中文描述 (仅显示, 不参与黄金) */
    int         defval;  /* 默认值 (用户原式 DC_MACRO_* 宏) */
    uint8_t     side;    /* 窗口侧别: DC_SIDE_IN / DC_SIDE_OUT */
} dc_sig_ent_t;

static const dc_sig_ent_t g_sig_table[DC_SIG_COUNT] = {
    { "INPUT",  "DC 输入信号值",       (int)DC_MACRO_INPUT,           DC_SIDE_IN  },
    { "OUTPUT", "DC 输出信号值",       (int)DC_MACRO_OUTPUT,          DC_SIDE_OUT },
    { "MAXIN",  "输入信号最大值",      (int)DC_MACRO_MAXIN,           DC_SIDE_IN  },
    { "MININ",  "输入信号最小值",      (int)DC_MACRO_MININ,           DC_SIDE_IN  },
    { "MAXOUT", "输出信号最大值",      (int)DC_MACRO_MAXOUT,          DC_SIDE_OUT },
    { "MINOUT", "输出信号最小值",      (int)DC_MACRO_MINOUT,          DC_SIDE_OUT },
    { "HILITE", "最高时序/中继信号值", (int)DC_MACRO_HIGH_LITE,       DC_SIDE_IN  },
    { "LOLITE", "最低时序/中继信号值", (int)DC_MACRO_LOW_LITE,        DC_SIDE_IN  }
};

/* 组合数据帧表 (user: DC_DATA_1..7, 位或组合) */
static const uint32_t g_data_table[DC_DATA_COUNT] = {
    (uint32_t)DC_DATA_1, (uint32_t)DC_DATA_2, (uint32_t)DC_DATA_3,
    (uint32_t)DC_DATA_4, (uint32_t)DC_DATA_5, (uint32_t)DC_DATA_6,
    (uint32_t)DC_DATA_7
};

uint8_t hw_dc_sig_count(void) { return (uint8_t)DC_SIG_COUNT; }

const char* hw_dc_sig_name(uint8_t sig)
{
    return (sig < DC_SIG_COUNT) ? g_sig_table[sig].name : "?";
}

const char* hw_dc_sig_desc(uint8_t sig)
{
    return (sig < DC_SIG_COUNT) ? g_sig_table[sig].desc : "?";
}

const char* hw_dc_side_name(uint8_t side)
{
    return (side == DC_SIDE_OUT) ? "OUT" : ((side == DC_SIDE_IN) ? "IN" : "?");
}

uint8_t hw_dc_data_count(void) { return (uint8_t)DC_DATA_COUNT; }

uint32_t hw_dc_data(uint8_t idx)
{
    return (idx < DC_DATA_COUNT) ? g_data_table[idx] : 0u;
}

/* 参考预值 (用户原式): 输入侧 0x059 / 输出侧 0x0080 */
int hw_dc_base_signal(uint8_t side)
{
    if (side == DC_SIDE_OUT) return (int)DC_MACRO_OUTPUT_BASE_SIGNAL;
    if (side == DC_SIDE_IN)  return (int)DC_MACRO_INPUT_BASE_SIGNAL;
    return -1;
}
/* FNV-1a-32 over (信号表 name + defval 低/高字节) + (数据表每项 4B LE)
 * — 与 Python 独立实现对拍锁定 (2026-10-01) */
uint32_t hw_dc_checksum(void)
{
    uint32_t h = 0x811C9DC5u;
    uint32_t i;
    for (i = 0; i < DC_SIG_COUNT; i++) {
        const unsigned char* s = (const unsigned char*)g_sig_table[i].name;
        while (*s) { h ^= (uint32_t)*s++; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
        h ^= (uint32_t)(g_sig_table[i].defval & 0xFF);
        h = (h * 0x01000193u) & 0xFFFFFFFFu;
        h ^= (uint32_t)((g_sig_table[i].defval >> 8) & 0xFF);
        h = (h * 0x01000193u) & 0xFFFFFFFFu;
    }
    for (i = 0; i < DC_DATA_COUNT; i++) {
        uint32_t v = g_data_table[i];
        uint32_t k;
        for (k = 0; k < 4u; k++) {
            h ^= ((v >> (8u * k)) & 0xFFu);
            h = (h * 0x01000193u) & 0xFFFFFFFFu;
        }
    }
    return h;
}

/* ============================================================
 * 3. 信号状态 + BSP 回调注入
 *    g_sig[] = 本地信号表 (无 BSP 时的取值源; set 写入此处)
 *    装了 BSP → hw_dc_sig_value 走实时读 (真机 ADC/GPIO)
 * ============================================================ */
static int            g_sig[DC_SIG_COUNT];
static hw_dc_bsp_t    g_bsp;      /* 全零 = 未装 (默认模拟) */

/* 默认模拟读取 (无 BSP): 直接给本地信号表 (确定性, 可回归) */
static int dc_sim_read(int sig)
{
    if (sig < 0 || sig >= (int)DC_SIG_COUNT) return -1;
    return g_sig[sig];
}

static int dc_sim_base(int side)
{
    return hw_dc_base_signal((uint8_t)side);
}

/* 参考预值读取: 装了 BSP 走实时读, 否则回内置 (与 hw_dc_sig_value 同款双路) */
int hw_dc_base_read(uint8_t side)
{
    if (side > DC_SIDE_OUT) return -1;
    if (g_bsp.base_read) {
        int v = g_bsp.base_read((int)side);
        return (v < 0) ? -1 : v;
    }
    return dc_sim_base((int)side);
}

int hw_dc_sig_value(uint8_t sig)
{
    if (sig >= DC_SIG_COUNT) return -1;
    if (g_bsp.read) {
        int v = g_bsp.read((int)sig);   /* 真机读数; <0 = 读失败 */
        return (v < 0) ? -1 : v;
    }
    return dc_sim_read((int)sig);
}

int hw_dc_sig_set(uint8_t sig, int val)
{
    if (sig >= DC_SIG_COUNT) return -1;
    g_sig[sig] = val;
    return 0;
}

void hw_dc_bsp_install(const hw_dc_bsp_t* bsp)
{
    if (bsp) { g_bsp = *bsp; }                                  /* 拷成静态副本 (勿存调用方栈指针) */
    else     { g_bsp.read = NULL; g_bsp.base_read = NULL; }     /* NULL = 卸载回模拟 */
}

void hw_dc_init(void* arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < DC_SIG_COUNT; i++) g_sig[i] = g_sig_table[i].defval;
    hw_dc_proto_reset();   /* 供电协议会话随上电复位 (内部状态, 非环境绑定) */
    /* ⚠️ 有意「不」清 g_bsp: BSP = 设备级硬件绑定 (ADC/GPIO 引脚映射),
     *    属「环境」而非本模块内部状态。上电复位只清信号表, 硬件绑定保持;
     *    卸载硬件只走 hw_dc_bsp_install(NULL)。
     *    真机陷阱 (2026-10-01 修): kvm_run 上电会自动调 hw_dc_init ——
     *    若此处把 BSP 抹掉, 真机装好的 ADC/GPIO 绑定会被 VM 启动悄悄清空,
     *    HUD 从此显示模拟值却「看着一切正常」(典型的假成功)。
     *    回归证据: selftest [10] 断言 init 后 BSP 仍在。 */
}

void hw_dc_hook(void* arg) { (void)arg; }   /* 无动态资源 */

/* 极值窗口: 输入侧信号 → [MININ,MAXIN]; 输出侧 → [MINOUT,MAXOUT] */
static void dc_window(uint8_t sig, int* lo, int* hi)
{
    if (g_sig_table[sig].side == DC_SIDE_OUT) {
        *lo = g_sig[DC_MINOUT]; *hi = g_sig[DC_MAXOUT];
    } else {
        *lo = g_sig[DC_MININ];  *hi = g_sig[DC_MAXIN];
    }
}

int hw_dc_in_range(uint8_t sig)
{
    int lo, hi, v;
    if (sig >= DC_SIG_COUNT) return 0;
    dc_window(sig, &lo, &hi);
    v = hw_dc_sig_value(sig);
    if (v < 0) return 0;                 /* 读失败 → 视为越界 */
    return (v >= lo && v <= hi) ? 1 : 0;
}

/* ============================================================
 * 4. 用户原槽位 API (语义修复: 判 NULL)
 * ============================================================ */
void dc_create_mode(int* args)
{
    uint32_t i;
    if (!args) return;
    for (i = 0; i < DC_SIG_COUNT; i++) g_sig[i] = args[i];
}

int dc_ready(int* point, int* offset)
{
    int idx, lo, hi, v;
    if (!point || !offset) return -1;
    idx = *offset;
    if (idx < 0 || idx >= (int)DC_SIG_COUNT) return -1;
    dc_window((uint8_t)idx, &lo, &hi);
    v = point[idx];
    return (v >= lo && v <= hi) ? 1 : 0;
}

void dc_init(int* handler, int* point)
{
    uint32_t i;
    if (handler) for (i = 0; i < DC_SIG_COUNT; i++) handler[i] = g_sig[i];
    if (point)   point[0] = (int)DC_DATA_5;   /* 组合数据帧 (输入|输出参考预值) */
}

int* dc_mode_input(void)
{
    return g_sig;   /* 指向模块内静态数组, 请勿释放 */
}

/* ============================================================
 * 4b. DCPP 供电协议 (帧层, 2026-10-01)
 *
 * 结构: 信号层之上的一层「字节流帧协议」——
 *   START(80 EF 02) | VER | SIDE | CMD | LEN | PAYLOAD | CKSUM8 | END(ED FF 0D)
 * 会话: OPEN 开启 (会话内才放行 GET/SET/BASE/RANGE/DATA), CLOSE 关断。
 * 纯整数算法 → 六模式行为逐位一致; 黄金值独立 Python 锁定 0x9432D6A5。
 * ============================================================ */

/* 命令表 (黄金折叠对象: 顺序/名/编号 均参与; 追加须重算 HW_DCPP_GOLDEN) */
typedef struct { uint8_t id; const char* name; } dc_dcpp_cmd_t;
static const dc_dcpp_cmd_t g_dcpp_cmd[DCPP_CMD_NUM] = {
    { DCPP_CMD_PING,  "PING"  }, { DCPP_CMD_COUNT, "COUNT" },
    { DCPP_CMD_GET,   "GET"   }, { DCPP_CMD_SET,   "SET"   },
    { DCPP_CMD_BASE,  "BASE"  }, { DCPP_CMD_RANGE, "RANGE" },
    { DCPP_CMD_DATA,  "DATA"  }, { DCPP_CMD_OPEN,  "OPEN"  },
    { DCPP_CMD_CLOSE, "CLOSE" }
};

static const char* const g_dcpp_state_name[5] = {
    "IDLE", "HDR", "PAYLOAD", "CKSUM", "END"
};

/* 帧定界字节 (小端): START 0x02EF80 / END 0x0DFFED */
static const uint8_t g_dcpp_start[HW_DCPP_END_LEN] = { 0x80u, 0xEFu, 0x02u };
static const uint8_t g_dcpp_end[HW_DCPP_END_LEN]   = { 0xEDu, 0xFFu, 0x0Du };

/* FNV-1a-32 增量 (黄金折叠 + 帧校验共用) */
static uint32_t dc_fnv1a(const unsigned char* p, uint32_t n, uint32_t h)
{
    uint32_t i;
    for (i = 0; i < n; i++) { h ^= (uint32_t)p[i]; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
    return h;
}

/* 帧校验: FNV-1a-32(VER,SIDE,CMD,LEN,PAYLOAD) 折叠 4 字节异或 → 1 字节 */
static uint8_t dc_cksum8(const uint8_t* d, uint8_t n)
{
    uint32_t h = dc_fnv1a((const unsigned char*)d, (uint32_t)n, 0x811C9DC5u);
    return (uint8_t)((h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24)) & 0xFFu);
}

/* 协议黄金: 命令表(名+编号) + START/END 常量 + (VER,HDR_LEN,MAX_PAYLOAD) */
uint32_t hw_dc_proto_checksum(void)
{
    uint32_t h = 0x811C9DC5u;
    uint32_t i;
    uint8_t  meta[3];
    for (i = 0; i < DCPP_CMD_NUM; i++) {
        const unsigned char* s = (const unsigned char*)g_dcpp_cmd[i].name;
        h = dc_fnv1a(s, (uint32_t)strlen((const char*)s), h);
        h = dc_fnv1a(&g_dcpp_cmd[i].id, 1u, h);
    }
    h = dc_fnv1a(g_dcpp_start, HW_DCPP_END_LEN, h);
    h = dc_fnv1a(g_dcpp_end,   HW_DCPP_END_LEN, h);
    meta[0] = (uint8_t)HW_DCPP_VER;
    meta[1] = (uint8_t)HW_DCPP_HDR_LEN;
    meta[2] = (uint8_t)HW_DCPP_MAX_PAYLOAD;
    h = dc_fnv1a(meta, 3u, h);
    return h;
}

uint8_t hw_dc_proto_ver(void)     { return (uint8_t)HW_DCPP_VER; }
uint8_t hw_dc_proto_cmd_num(void) { return (uint8_t)DCPP_CMD_NUM; }

const char* hw_dc_proto_cmd_name(uint8_t cmd)
{
    uint32_t i;
    for (i = 0; i < DCPP_CMD_NUM; i++)
        if (g_dcpp_cmd[i].id == cmd) return g_dcpp_cmd[i].name;
    return "?";
}

const char* hw_dc_proto_state_name(uint8_t st)
{
    return (st < 5u) ? g_dcpp_state_name[st] : "?";
}

int hw_dc_proto_frame_len(uint8_t len)
{
    if (len > (uint8_t)HW_DCPP_MAX_PAYLOAD) return -1;
    return (int)(HW_DCPP_HDR_LEN + HW_DCPP_CKSUM_LEN + HW_DCPP_END_LEN + len);
}

/* ---- 会话/状态机静态态 (单线程约定, 家族纪律坑#12) ---- */
static uint8_t  g_dcpp_state  = (uint8_t)HW_DCPP_IDLE;
static uint8_t  g_dcpp_rx[HW_DCPP_MAX_FRAME];
static uint8_t  g_dcpp_rxlen  = 0;
static uint8_t  g_dcpp_endi   = 0;
static uint8_t  g_dcpp_resp[8];
static uint8_t  g_dcpp_resplen = 0;
static int      g_dcpp_respval = -1;
static uint8_t  g_dcpp_opened = 0;
static uint32_t g_dcpp_rx_cnt = 0, g_dcpp_tx_cnt = 0, g_dcpp_err_cnt = 0;
static uint8_t  g_dcpp_last   = 0;

/* 只复位收帧状态 (保留应答, 供 parse 返回) */
static void dc_proto_reset_rx(void)
{
    g_dcpp_state = (uint8_t)HW_DCPP_IDLE;
    g_dcpp_rxlen = 0;
    g_dcpp_endi  = 0;
}

/* 复位收帧状态 + 应答 */
static void dc_proto_reset_sm(void)
{
    dc_proto_reset_rx();
    g_dcpp_resplen = 0;
    g_dcpp_respval = -1;
}

void hw_dc_proto_reset(void)
{
    dc_proto_reset_sm();
    g_dcpp_opened = 0;
    g_dcpp_rx_cnt = g_dcpp_tx_cnt = g_dcpp_err_cnt = 0;
    g_dcpp_last = 0;
}

uint8_t hw_dc_proto_state(void) { return g_dcpp_state; }
int     hw_dc_proto_opened(void) { return (int)g_dcpp_opened; }

void hw_dc_proto_stats(uint32_t* rx, uint32_t* tx, uint32_t* err, uint8_t* last)
{
    if (rx)   *rx   = g_dcpp_rx_cnt;
    if (tx)   *tx   = g_dcpp_tx_cnt;
    if (err)  *err  = g_dcpp_err_cnt;
    if (last) *last = g_dcpp_last;
}

int hw_dc_proto_resp(uint8_t* out, int cap)
{
    int n = (int)g_dcpp_resplen, i;
    if (!out || cap <= 0) return 0;
    for (i = 0; i < n && i < cap; i++) out[i] = g_dcpp_resp[i];
    return n;
}

int hw_dc_proto_resp_value(void) { return g_dcpp_respval; }

/* 应答写入 (同步维护 resp_value = resp[0] | resp[1]<<8) */
static void dc_proto_put1(uint8_t a)
{
    g_dcpp_resp[0] = a; g_dcpp_resplen = 1;
    g_dcpp_respval = (int)a;
}
static void dc_proto_put2(uint8_t a, uint8_t b)
{
    g_dcpp_resp[0] = a; g_dcpp_resp[1] = b; g_dcpp_resplen = 2;
    g_dcpp_respval = (int)a | ((int)b << 8);
}

/* 打帧: START|VER|SIDE|CMD|LEN|PL|CKSUM|END */
int hw_dc_proto_build(uint8_t cmd, uint8_t side, const uint8_t* pl,
                      uint8_t len, uint8_t* out, int cap)
{
    int total = hw_dc_proto_frame_len(len);
    uint8_t i;
    if (total < 0) return -1;
    if (!out || cap < total || (len > 0u && !pl)) return -1;
    if (hw_dc_proto_cmd_name(cmd)[0] == '?') return -1;   /* 未知命令 */
    for (i = 0; i < HW_DCPP_END_LEN; i++) out[i] = g_dcpp_start[i];
    out[3] = (uint8_t)HW_DCPP_VER;
    out[4] = side;
    out[5] = cmd;
    out[6] = len;
    for (i = 0; i < len; i++) out[7 + i] = pl[i];
    out[7 + len] = dc_cksum8(out + 3, (uint8_t)(4u + len));
    for (i = 0; i < HW_DCPP_END_LEN; i++)
        out[8 + len + i] = g_dcpp_end[i];
    return total;
}

/* 派发一条已收齐的帧 (g_dcpp_rx) → 0 成功 / 负错误码 */
static int dc_proto_dispatch(void)
{
    uint8_t cmd  = g_dcpp_rx[5];
    uint8_t len  = g_dcpp_rx[6];
    const uint8_t* pl = g_dcpp_rx + HW_DCPP_HDR_LEN;
    int need_open;

    g_dcpp_resplen = 0; g_dcpp_respval = -1;

    /* 先判命令合法性 (结构错误, 与会话无关) */
    if (hw_dc_proto_cmd_name(cmd)[0] == '?') return HW_DCPP_ERR_CMD;

    /* 会话门禁: OPEN/CLOSE/PING/COUNT 不需会话, 其余必须在会话内 */
    need_open = (cmd != DCPP_CMD_PING && cmd != DCPP_CMD_COUNT &&
                 cmd != DCPP_CMD_OPEN && cmd != DCPP_CMD_CLOSE);
    if (need_open && !g_dcpp_opened) return HW_DCPP_ERR_CLOSED;

    switch (cmd) {
    case DCPP_CMD_PING:
        if (len != 0u) return HW_DCPP_ERR_ARG;
        dc_proto_put1((uint8_t)HW_DCPP_PONG);
        break;
    case DCPP_CMD_COUNT:
        if (len != 0u) return HW_DCPP_ERR_ARG;
        dc_proto_put2((uint8_t)DC_SIG_COUNT, (uint8_t)DC_DATA_COUNT);
        break;
    case DCPP_CMD_OPEN:
        if (len != 0u) return HW_DCPP_ERR_ARG;
        g_dcpp_opened = 1; dc_proto_put1(1u);
        break;
    case DCPP_CMD_CLOSE:
        if (len != 0u) return HW_DCPP_ERR_ARG;
        g_dcpp_opened = 0; dc_proto_put1(0u);
        break;
    case DCPP_CMD_GET: {
        int v;
        if (len != 1u || pl[0] >= (uint8_t)DC_SIG_COUNT) return HW_DCPP_ERR_ARG;
        v = hw_dc_sig_value(pl[0]);
        dc_proto_put2((uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF));
        break;
    }
    case DCPP_CMD_SET: {
        int rc;
        if (len != 3u || pl[0] >= (uint8_t)DC_SIG_COUNT) return HW_DCPP_ERR_ARG;
        rc = hw_dc_sig_set(pl[0], (int)(int16_t)((int)pl[1] | ((int)pl[2] << 8)));
        dc_proto_put1((uint8_t)(rc == 0 ? 0u : 0xFFu));
        break;
    }
    case DCPP_CMD_BASE: {
        int v;
        if (len != 1u || pl[0] > (uint8_t)DC_SIDE_OUT) return HW_DCPP_ERR_ARG;
        v = hw_dc_base_read(pl[0]);
        dc_proto_put2((uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF));
        break;
    }
    case DCPP_CMD_RANGE:
        if (len != 1u || pl[0] >= (uint8_t)DC_SIG_COUNT) return HW_DCPP_ERR_ARG;
        dc_proto_put1((uint8_t)hw_dc_in_range(pl[0]));
        break;
    case DCPP_CMD_DATA: {
        uint32_t v;
        if (len != 1u || pl[0] >= (uint8_t)DC_DATA_COUNT) return HW_DCPP_ERR_ARG;
        v = hw_dc_data(pl[0]);
        dc_proto_put2((uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF));
        break;
    }
    default:
        return HW_DCPP_ERR_CMD;
    }
    return 0;
}

int hw_dc_proto_feed(uint8_t byte)
{
    switch (g_dcpp_state) {
    case HW_DCPP_IDLE:
        if (byte != g_dcpp_start[0]) { g_dcpp_err_cnt++; return HW_DCPP_ERR_FRAME; }
        g_dcpp_rx[0] = byte; g_dcpp_rxlen = 1;
        g_dcpp_state = (uint8_t)HW_DCPP_HDR;
        return HW_DCPP_FEED_MORE;
    case HW_DCPP_HDR:
        if (g_dcpp_rxlen >= (uint8_t)HW_DCPP_HDR_LEN) {   /* 防御 (不应发生) */
            dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_FRAME;
        }
        g_dcpp_rx[g_dcpp_rxlen++] = byte;
        if (g_dcpp_rxlen < (uint8_t)HW_DCPP_HDR_LEN) return HW_DCPP_FEED_MORE;
        /* 收齐 7B: 校验定界 + VER + LEN */
        if (g_dcpp_rx[1] != g_dcpp_start[1] || g_dcpp_rx[2] != g_dcpp_start[2] ||
            g_dcpp_rx[3] != (uint8_t)HW_DCPP_VER) {
            dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_FRAME;
        }
        if (g_dcpp_rx[6] > (uint8_t)HW_DCPP_MAX_PAYLOAD) {
            dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_LEN;
        }
        g_dcpp_state = (g_dcpp_rx[6] == 0u) ? (uint8_t)HW_DCPP_CKSUM
                                            : (uint8_t)HW_DCPP_PAYLOAD;
        return HW_DCPP_FEED_MORE;
    case HW_DCPP_PAYLOAD: {
        uint8_t len = g_dcpp_rx[6];
        if (g_dcpp_rxlen >= (uint8_t)(HW_DCPP_HDR_LEN + len)) {
            dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_FRAME;
        }
        g_dcpp_rx[g_dcpp_rxlen++] = byte;
        if (g_dcpp_rxlen >= (uint8_t)(HW_DCPP_HDR_LEN + len))
            g_dcpp_state = (uint8_t)HW_DCPP_CKSUM;
        return HW_DCPP_FEED_MORE;
    }
    case HW_DCPP_CKSUM: {
        uint8_t len = g_dcpp_rx[6];
        uint8_t ck;
        g_dcpp_rx[g_dcpp_rxlen++] = byte;                  /* index 7+len */
        ck = dc_cksum8(g_dcpp_rx + 3, (uint8_t)(4u + len));
        g_dcpp_endi = 0;
        g_dcpp_state = (uint8_t)HW_DCPP_END;
        if (ck != byte) { dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_CKSUM; }
        return HW_DCPP_FEED_MORE;
    }
    case HW_DCPP_END: {
        uint8_t len = g_dcpp_rx[6];
        int rc;
        g_dcpp_rx[g_dcpp_rxlen++] = byte;                  /* index 8+len+endi */
        if (g_dcpp_endi >= (uint8_t)HW_DCPP_END_LEN ||
            byte != g_dcpp_end[g_dcpp_endi]) {
            dc_proto_reset_sm(); g_dcpp_err_cnt++; return HW_DCPP_ERR_END;
        }
        if (++g_dcpp_endi < (uint8_t)HW_DCPP_END_LEN) return HW_DCPP_FEED_MORE;
        (void)len;
        rc = dc_proto_dispatch();
        if (rc < 0) { dc_proto_reset_sm(); g_dcpp_err_cnt++; return rc; }
        g_dcpp_last = g_dcpp_rx[5];
        g_dcpp_rx_cnt++;
        if (g_dcpp_resplen) g_dcpp_tx_cnt++;
        dc_proto_reset_rx();       /* 回 IDLE; 保留应答/opened/计数/last */
        return HW_DCPP_FEED_FRAME;
    }
    default:
        dc_proto_reset_sm();
        g_dcpp_err_cnt++;
        return HW_DCPP_ERR_FRAME;
    }
}

int hw_dc_proto_parse(const uint8_t* in, int len)
{
    int i, got = 0;
    if (!in || len <= 0) return HW_DCPP_ERR_ARG;
    dc_proto_reset_sm();                 /* 单帧解析: 从干净状态开始 */
    for (i = 0; i < len; i++) {
        int r = hw_dc_proto_feed(in[i]);
        if (r < 0) return r;
        if (r == HW_DCPP_FEED_FRAME) got = 1;
    }
    return got ? (int)g_dcpp_resplen : HW_DCPP_ERR_FRAME;
}

/* 默认负载 (供 CLI/命令通道一条命令打帧; 让 .mo 端到端值可判别) */
static int dc_proto_build_cmd(uint8_t cmd, uint8_t* out, int cap)
{
    uint8_t pl[HW_DCPP_MAX_PAYLOAD];
    uint8_t n = 0;
    uint8_t side = (uint8_t)DC_SIDE_IN;

    switch (cmd) {
    case DCPP_CMD_PING: case DCPP_CMD_COUNT:
    case DCPP_CMD_OPEN: case DCPP_CMD_CLOSE:
        n = 0; break;
    case DCPP_CMD_GET:   case DCPP_CMD_RANGE: pl[0] = 0; n = 1; break;
    case DCPP_CMD_BASE:  pl[0] = (uint8_t)DC_SIDE_OUT; n = 1; break;
    case DCPP_CMD_DATA:  pl[0] = 4; n = 1; break;
    case DCPP_CMD_SET:   pl[0] = 0; pl[1] = 42; pl[2] = 0; n = 3; break;
    default: return -1;
    }
    return hw_dc_proto_build(cmd, side, pl, n, out, cap);
}

/* 打帧 → 逐字节喂入 (会话随之推进); 返回 feed 末字节结果 (1 / 负错误码) */
static int dc_proto_run_cmd(uint8_t cmd)
{
    uint8_t frame[HW_DCPP_MAX_FRAME];
    int n = dc_proto_build_cmd(cmd, frame, (int)sizeof(frame));
    int i, r = HW_DCPP_FEED_MORE;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) { r = hw_dc_proto_feed(frame[i]); if (r < 0) return r; }
    return r;
}

/* ============================================================
 * 5. 命令分发 (VM OP_HW_DC_CALL / CLI 一次性)
 *    返回: >=0 结果; -1 未识别/参数非法; -2 help
 * ============================================================ */
int hw_dc_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (!cmd) return -1;
    if (strcmp(cmd, "count") == 0)     return (int)DC_SIG_COUNT;
    if (strcmp(cmd, "datacount") == 0) return (int)DC_DATA_COUNT;
    if (strcmp(cmd, "ok") == 0)
        return (hw_dc_checksum() == HW_DC_GOLDEN) ? 1 : 0;
    if (strcmp(cmd, "mode") == 0)      return (int)hw_dc_mode();
    if (strcmp(cmd, "help") == 0)      return -2;
    if (strncmp(cmd, "sig ", 4) == 0) {
        long sig = strtol(cmd + 4, NULL, 10);
        if (sig < 0 || sig >= (long)DC_SIG_COUNT) return -1;
        return hw_dc_sig_value((uint8_t)sig);
    }
    if (strncmp(cmd, "name ", 5) == 0) {
        long sig = strtol(cmd + 5, NULL, 10);
        if (sig < 0 || sig >= (long)DC_SIG_COUNT) return -1;
        return (int)(unsigned char)g_sig_table[sig].name[0];   /* 首字节 (命令通道返回码) */
    }
    if (strncmp(cmd, "set ", 4) == 0) {
        long sig = 0, val = 0;
        if (sscanf(cmd + 4, "%ld %ld", &sig, &val) != 2) return -1;
        if (sig < 0 || sig >= (long)DC_SIG_COUNT) return -1;
        return hw_dc_sig_set((uint8_t)sig, (int)val);
    }
    if (strncmp(cmd, "base ", 5) == 0) {
        long side = strtol(cmd + 5, NULL, 10);
        if (side < 0 || side > 1) return -1;
        return hw_dc_base_read((uint8_t)side);   /* BSP 优先, 否则内置预值 */
    }
    if (strncmp(cmd, "range ", 6) == 0) {
        long sig = strtol(cmd + 6, NULL, 10);
        if (sig < 0 || sig >= (long)DC_SIG_COUNT) return -1;
        return hw_dc_in_range((uint8_t)sig);
    }
    if (strncmp(cmd, "data ", 5) == 0) {
        long idx = strtol(cmd + 5, NULL, 10);
        if (idx < 0 || idx >= (long)DC_DATA_COUNT) return -1;
        return (int)g_data_table[idx];
    }
    /* ---- DCPP 供电协议 (帧层) ---- */
    if (strcmp(cmd, "protover") == 0)   return (int)hw_dc_proto_ver();
    if (strcmp(cmd, "protocount") == 0) return (int)hw_dc_proto_cmd_num();
    if (strcmp(cmd, "protook") == 0)
        return (hw_dc_proto_checksum() == HW_DCPP_GOLDEN) ? 1 : 0;
    if (strcmp(cmd, "protostat") == 0)  return (int)hw_dc_proto_state();
    if (strcmp(cmd, "protovalue") == 0) return hw_dc_proto_resp_value();
    if (strcmp(cmd, "protoopened") == 0) return hw_dc_proto_opened();
    if (strcmp(cmd, "protoreset") == 0) { hw_dc_proto_reset(); return 0; }
    if (strncmp(cmd, "protoframe ", 11) == 0) {          /* 帧长 = 11+len */
        long len = strtol(cmd + 11, NULL, 10);
        if (len < 0 || len > (long)HW_DCPP_MAX_PAYLOAD) return -1;
        return hw_dc_proto_frame_len((uint8_t)len);
    }
    if (strncmp(cmd, "protobuild ", 11) == 0) {          /* 命令 N → 帧长 */
        uint8_t frame[HW_DCPP_MAX_FRAME];
        long c = strtol(cmd + 11, NULL, 10);
        if (c <= 0 || c > (long)DCPP_CMD_NUM) return -1;
        return dc_proto_build_cmd((uint8_t)c, frame, (int)sizeof(frame));
    }
    if (strncmp(cmd, "protocmd ", 9) == 0) {             /* 命令 N → 喂完整帧 */
        long c = strtol(cmd + 9, NULL, 10);
        if (c <= 0 || c > (long)DCPP_CMD_NUM) return -1;
        return dc_proto_run_cmd((uint8_t)c);
    }
    return -1;
}

/* ============================================================
 * 6. 自检 (全跨式黄金锁定)
 * ============================================================ */
#define DC_CHECK(cond) do { if (!(cond)) fails++; } while (0)

/* BSP 注入桩 (selftest 用): 返回固定虚拟读数, 验证「装了 BSP 走实时读」 */
static int dc_test_read(int sig) { return 100 + sig; }
static int dc_test_base(int side) { return (side == 0) ? 0x111 : 0x222; }

int hw_dc_selftest(int (*putf)(const char*))
{
    static char line[192];                 /* >512B 禁; static 化 (家族纪律坑#12) */
    int fails = 0;
    uint32_t i;

    /* [1] 黄金校验和锁定 */
    DC_CHECK(hw_dc_checksum() == HW_DC_GOLDEN);

    /* [2] 信号表: 计数/名/描述/默认值 (与宏逐项对拍) + 越界容错 */
    DC_CHECK(hw_dc_sig_count() == (uint8_t)DC_SIG_COUNT);
    DC_CHECK(strcmp(hw_dc_sig_name(DC_INPUT),  "INPUT")  == 0);
    DC_CHECK(strcmp(hw_dc_sig_name(DC_OUTPUT), "OUTPUT") == 0);
    DC_CHECK(strcmp(hw_dc_sig_name(DC_LOW_LITE), "LOLITE") == 0);
    DC_CHECK(strcmp(hw_dc_sig_name(DC_SIG_COUNT), "?") == 0);
    DC_CHECK(strcmp(hw_dc_sig_desc(DC_HIGH_LITE), "最高时序/中继信号值") == 0 ||
             hw_dc_sig_desc(DC_HIGH_LITE) != NULL);
    DC_CHECK(strcmp(hw_dc_side_name(DC_SIDE_IN), "IN") == 0);
    DC_CHECK(strcmp(hw_dc_side_name(DC_SIDE_OUT), "OUT") == 0);
    DC_CHECK(strcmp(hw_dc_side_name(9), "?") == 0);

    /* [3] 参考预值 (用户原式): IN=0x059 / OUT=0x0080 */
    DC_CHECK(hw_dc_base_signal(DC_SIDE_IN)  == 0x059);
    DC_CHECK(hw_dc_base_signal(DC_SIDE_OUT) == 0x0080);
    DC_CHECK(hw_dc_base_signal(9) == -1);

    /* [4] 组合数据帧表: 7 项, 与宏对拍 (DC_DATA_5=0x0D9 等) */
    DC_CHECK(hw_dc_data_count() == (uint8_t)DC_DATA_COUNT);
    DC_CHECK(hw_dc_data(0) == (uint32_t)DC_DATA_1);
    DC_CHECK(hw_dc_data(3) == (uint32_t)DC_DATA_4);   /* 0x003 */
    DC_CHECK(hw_dc_data(4) == (uint32_t)DC_DATA_5);   /* 0x0D9 */
    DC_CHECK(hw_dc_data(5) == (uint32_t)DC_DATA_6);   /* 0x059 */
    DC_CHECK(hw_dc_data(6) == (uint32_t)DC_DATA_7);   /* 0x080 */
    DC_CHECK(hw_dc_data((uint8_t)DC_DATA_COUNT) == 0u);
    DC_CHECK((uint32_t)DC_DATA_5 == 0x0D9u);

    /* [5] get/set 往返 + 越界容错 (默认模拟读) */
    hw_dc_init(NULL);
    DC_CHECK(hw_dc_sig_value(DC_INPUT) == (int)DC_MACRO_INPUT);
    DC_CHECK(hw_dc_sig_value(DC_HIGH_LITE) == 0x01);
    DC_CHECK(hw_dc_sig_value(DC_SIG_COUNT) == -1);
    DC_CHECK(hw_dc_sig_set(DC_INPUT, 50) == 0);
    DC_CHECK(hw_dc_sig_value(DC_INPUT) == 50);
    DC_CHECK(hw_dc_sig_set((uint8_t)DC_SIG_COUNT, 1) == -1);

    /* [6] 极值窗口判决: 默认窗口 [MININ,MAXIN]=[0,0] → 越界; 放宽后命中 */
    hw_dc_init(NULL);
    DC_CHECK(hw_dc_in_range(DC_INPUT) == 1);          /* 0 落在 [0,0] 内 */
    DC_CHECK(hw_dc_sig_set(DC_INPUT, 50) == 0);
    DC_CHECK(hw_dc_in_range(DC_INPUT) == 0);          /* 50 越界 */
    DC_CHECK(hw_dc_sig_set(DC_MAXIN, 100) == 0);
    DC_CHECK(hw_dc_in_range(DC_INPUT) == 1);          /* 50 落在 [0,100] */
    hw_dc_init(NULL);

    /* [7] 用户原槽位 API: create_mode / init / mode_input / ready */
    {
        int args[DC_SIG_COUNT] = { 11, 22, 44, 33, 55, 66, 77, 88 };
        /* 索引: 0=INPUT=11 1=OUTPUT=22 2=MAXIN=44 3=MININ=33
         *       4=MAXOUT=55 5=MINOUT=66 6=HILITE=77 7=LOLITE=88
         * → 输入侧窗口 [MININ,MAXIN] = [33,44] */
        int handler[DC_SIG_COUNT];
        int frame[1] = { 0 };
        int off = 0;
        dc_create_mode(args);
        DC_CHECK(hw_dc_sig_value(DC_OUTPUT) == 22);
        DC_CHECK(dc_mode_input() != NULL && dc_mode_input()[7] == 88);
        dc_init(handler, frame);
        for (i = 0; i < DC_SIG_COUNT; i++) DC_CHECK(handler[i] == args[i]);
        DC_CHECK(frame[0] == (int)DC_DATA_5);
        DC_CHECK(dc_ready(NULL, &off) == -1);         /* NULL 容错 */
        DC_CHECK(dc_ready(handler, NULL) == -1);
        /* 窗口 [MININ,MAXIN]=[33,44] → INPUT(11) 越界 / MAXIN(44) 命中 */
        off = DC_INPUT;  DC_CHECK(dc_ready(handler, &off) == 0);
        off = DC_MAXIN;  DC_CHECK(dc_ready(handler, &off) == 1);
        off = 99;        DC_CHECK(dc_ready(handler, &off) == -1);
        dc_init(NULL, NULL);                          /* 容错 (不崩) */
        dc_create_mode(NULL);                         /* 容错 */
    }
    hw_dc_init(NULL);

    /* [8] 命令分发往返 */
    DC_CHECK(hw_dc_cmd("count", NULL) == (int)DC_SIG_COUNT);
    DC_CHECK(hw_dc_cmd("datacount", NULL) == (int)DC_DATA_COUNT);
    DC_CHECK(hw_dc_cmd("ok", NULL) == 1);
    DC_CHECK(hw_dc_cmd("set 0 42", NULL) == 0);
    DC_CHECK(hw_dc_cmd("sig 0", NULL) == 42);
    DC_CHECK(hw_dc_cmd("data 4", NULL) == (int)DC_DATA_5);
    DC_CHECK(hw_dc_cmd("base 0", NULL) == 0x059);
    DC_CHECK(hw_dc_cmd("base 1", NULL) == 0x080);
    DC_CHECK(hw_dc_cmd("mode", NULL) == (int)hw_dc_mode());
    DC_CHECK(hw_dc_mode() < HW_DC_MODE_MAX);
    DC_CHECK(hw_dc_cmd("bogus", NULL) == -1);
    DC_CHECK(hw_dc_cmd("sig 99", NULL) == -1);
    DC_CHECK(hw_dc_cmd("help", NULL) == -2);
    DC_CHECK(hw_dc_cmd(NULL, NULL) == -1);
    hw_dc_init(NULL);

    /* [9] BSP 注入: 装了回调走实时读 (非空转); 卸载回模拟 */
    {
        hw_dc_bsp_t bsp;
        bsp.read = dc_test_read;
        bsp.base_read = dc_test_base;
        hw_dc_bsp_install(&bsp);
        DC_CHECK(hw_dc_sig_value(DC_INPUT) == 100);       /* 100 + 0 */
        DC_CHECK(hw_dc_sig_value(DC_LOW_LITE) == 107);    /* 100 + 7 */
        DC_CHECK(hw_dc_base_read(DC_SIDE_IN) == 0x111);   /* BSP 覆盖参考预值 */
        DC_CHECK(hw_dc_base_read(DC_SIDE_OUT) == 0x222);
        DC_CHECK(hw_dc_cmd("base 0", NULL) == 0x111);     /* 命令通道也走 BSP */
        DC_CHECK(g_bsp.base_read != NULL &&
                 g_bsp.base_read(DC_SIDE_IN) == 0x111 &&
                 g_bsp.base_read(DC_SIDE_OUT) == 0x222);
        hw_dc_bsp_install(NULL);                          /* 卸载 */
        DC_CHECK(g_bsp.read == NULL);
        DC_CHECK(hw_dc_sig_value(DC_INPUT) == (int)DC_MACRO_INPUT);  /* 回表值 */
        DC_CHECK(hw_dc_base_read(DC_SIDE_IN) == 0x059);   /* 回内置预值 */
    }
    hw_dc_init(NULL);

    /* [10] 上电幂等: init 后信号/极值复位, 校验和恒定 */
    DC_CHECK(hw_dc_checksum() == HW_DC_GOLDEN);
    DC_CHECK(hw_dc_sig_value(DC_INPUT) == (int)DC_MACRO_INPUT);

    /* [11] 上电不做「静默卸载」: BSP = 设备级硬件绑定, init 必须保住它。
     *      真机陷阱 (2026-10-01): kvm_run 上电自动 init, 若它抹掉 BSP,
     *      真机 ADC/GPIO 绑定会被悄悄清空 → HUD 显示模拟值却一切「正常」。 */
    {
        hw_dc_bsp_t bsp10;
        bsp10.read = dc_test_read;
        bsp10.base_read = dc_test_base;
        hw_dc_bsp_install(&bsp10);
        DC_CHECK(hw_dc_sig_value(DC_INPUT) == 100);        /* 装后走真机读 */
        hw_dc_init(NULL);                                  /* 模拟 kvm_run 上电 */
        DC_CHECK(g_bsp.read == dc_test_read);              /* ← 必须仍绑定 */
        DC_CHECK(hw_dc_sig_value(DC_INPUT) == 100);        /* 仍走真机读, 非回表值 */
        DC_CHECK(hw_dc_base_read(DC_SIDE_IN) == 0x111);
        hw_dc_bsp_install(NULL);                           /* 只此一途可卸载 */
        DC_CHECK(hw_dc_sig_value(DC_INPUT) == (int)DC_MACRO_INPUT);
    }
    hw_dc_init(NULL);

    /* ============================================================
     * [12]~[16] DCPP 供电协议 (帧层): 黄金 / 打帧 / 解析 / 错误码 / 会话往返
     * ============================================================ */

    /* [12] 协议黄金 + 版本/命令表/名表 */
    DC_CHECK(hw_dc_proto_checksum() == HW_DCPP_GOLDEN);
    DC_CHECK(hw_dc_proto_ver() == (uint8_t)HW_DCPP_VER);
    DC_CHECK(hw_dc_proto_cmd_num() == (uint8_t)DCPP_CMD_NUM);
    DC_CHECK(strcmp(hw_dc_proto_cmd_name(DCPP_CMD_PING),  "PING")  == 0);
    DC_CHECK(strcmp(hw_dc_proto_cmd_name(DCPP_CMD_CLOSE), "CLOSE") == 0);
    DC_CHECK(strcmp(hw_dc_proto_cmd_name(0x7Fu), "?") == 0);
    DC_CHECK(strcmp(hw_dc_proto_state_name((uint8_t)HW_DCPP_IDLE), "IDLE") == 0);
    DC_CHECK(strcmp(hw_dc_proto_state_name(9), "?") == 0);

    /* [13] 帧长 + 打帧字节级对拍 (START/END 定界来自用户原式常量) */
    DC_CHECK(hw_dc_proto_frame_len(0)  == 11);
    DC_CHECK(hw_dc_proto_frame_len(16) == 27);
    DC_CHECK(hw_dc_proto_frame_len(17) == -1);
    {
        uint8_t fr[HW_DCPP_MAX_FRAME];
        uint8_t pl1[1];
        int fl;
        pl1[0] = 4;
        fl = hw_dc_proto_build(DCPP_CMD_DATA, (uint8_t)DC_SIDE_IN, pl1, 1,
                               fr, (int)sizeof(fr));
        DC_CHECK(fl == 12);
        DC_CHECK(fr[0] == 0x80 && fr[1] == 0xEF && fr[2] == 0x02);  /* START 小端 */
        DC_CHECK(fr[3] == (uint8_t)HW_DCPP_VER);
        DC_CHECK(fr[4] == (uint8_t)DC_SIDE_IN);
        DC_CHECK(fr[5] == (uint8_t)DCPP_CMD_DATA);
        DC_CHECK(fr[6] == 1 && fr[7] == 4);
        DC_CHECK(fr[9] == 0xED && fr[10] == 0xFF && fr[11] == 0x0D); /* END 小端 */
        /* 帧长恰好 = 11 + len, 定界符完整 */
        DC_CHECK(hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, fr, 4) == -1); /* 容量不足 */
        DC_CHECK(hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, (uint8_t)(HW_DCPP_MAX_PAYLOAD + 1),
                                   fr, (int)sizeof(fr)) == -1);              /* 负载超长 */
        DC_CHECK(hw_dc_proto_build(0x7F, 0, NULL, 0, fr, (int)sizeof(fr)) == -1); /* 未知命令 */
    }

    /* [14] 单帧解析: 应答长度/值 + 会话门禁 */
    hw_dc_init(NULL);
    {
        uint8_t fr[HW_DCPP_MAX_FRAME];
        int fl, n;
        uint8_t resp[8];

        /* 未开启会话: GET → ERR_CLOSED */
        {
            uint8_t pl1[1]; pl1[0] = 0;
            fl = hw_dc_proto_build(DCPP_CMD_GET, (uint8_t)DC_SIDE_IN, pl1, 1,
                                   fr, (int)sizeof(fr));
        }
        DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_CLOSED);
        DC_CHECK(hw_dc_proto_opened() == 0);

        /* OPEN → 会话开启, 应答 [1] */
        fl = hw_dc_proto_build(DCPP_CMD_OPEN, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(fl == 11);
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1);          /* 应答 1 字节 */
        DC_CHECK(hw_dc_proto_opened() == 1);
        DC_CHECK(hw_dc_proto_resp_value() == 1);
        DC_CHECK(strcmp(hw_dc_proto_state_name(hw_dc_proto_state()), "IDLE") == 0);

        /* COUNT → [信号数, 数据帧数] = 8 | 7<<8 = 1800 */
        fl = hw_dc_proto_build(DCPP_CMD_COUNT, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 2);
        DC_CHECK(hw_dc_proto_resp_value() == (8 | (7 << 8)));
        n = hw_dc_proto_resp(resp, (int)sizeof(resp));
        DC_CHECK(n == 2 && resp[0] == 8 && resp[1] == 7);

        /* PING → 应答 0x50 (不需会话) */
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1);
        DC_CHECK(hw_dc_proto_resp_value() == (int)HW_DCPP_PONG);

        /* DATA idx=4 → DATA_5 = 0x0D9 = 217 (2B 小端) */
        {
            uint8_t pl1[1]; pl1[0] = 4;
            fl = hw_dc_proto_build(DCPP_CMD_DATA, 0, pl1, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 2);
            DC_CHECK(hw_dc_proto_resp_value() == (int)DC_DATA_5);
            DC_CHECK(hw_dc_proto_resp_value() == 0x0D9);
        }
        /* BASE side=1(OUT) → 0x080 = 128 */
        {
            uint8_t pl1[1]; pl1[0] = (uint8_t)DC_SIDE_OUT;
            fl = hw_dc_proto_build(DCPP_CMD_BASE, 0, pl1, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 2);
            DC_CHECK(hw_dc_proto_resp_value() == 0x080);
        }
    }

    /* [15] 错误码全覆盖 (结构/校验/定界/命令/长度/参数/会话) */
    hw_dc_init(NULL);
    {
        uint8_t fr[HW_DCPP_MAX_FRAME];
        int fl;
        uint8_t pl[HW_DCPP_MAX_PAYLOAD];

        /* 起始定界错: 首字节非 0x80 */
        hw_dc_proto_reset();
        DC_CHECK(hw_dc_proto_feed(0x00) == HW_DCPP_ERR_FRAME);
        /* 起始定界错: 首字节对但第 2 字节错 */
        hw_dc_proto_reset();
        DC_CHECK(hw_dc_proto_feed(0x80) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x00) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x02) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x01) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x00) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed((uint8_t)DCPP_CMD_PING) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x00) == HW_DCPP_ERR_FRAME);   /* rx[1]=0x00 != 0xEF */

        /* 负载超长: LEN=32 > 16 → ERR_LEN */
        hw_dc_proto_reset();
        DC_CHECK(hw_dc_proto_feed(0x80) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0xEF) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x02) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x01) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x00) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed((uint8_t)DCPP_CMD_PING) == HW_DCPP_FEED_MORE);
        DC_CHECK(hw_dc_proto_feed(0x20) == HW_DCPP_ERR_LEN);

        /* 校验错: 打帧后翻转 CKSUM 字节 */
        hw_dc_proto_reset();
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(fl == 11);
        fr[7] = (uint8_t)(fr[7] ^ 0xFFu);
        DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_CKSUM);

        /* 结束定界错: 打帧后翻转 END 末字节 */
        hw_dc_proto_reset();
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, fr, (int)sizeof(fr));
        fr[10] = (uint8_t)(fr[10] ^ 0xFFu);
        DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_END);

        /* 未知命令: 用合法校验但命令号 0x7F 的帧 (手工构帧) */
        hw_dc_proto_reset();
        {
            uint8_t bad[HW_DCPP_MAX_FRAME];
            uint32_t hh;
            bad[0] = 0x80; bad[1] = 0xEF; bad[2] = 0x02;
            bad[3] = (uint8_t)HW_DCPP_VER; bad[4] = 0; bad[5] = 0x7F; bad[6] = 0;
            hh = dc_fnv1a(bad + 3, 4u, 0x811C9DC5u);
            bad[7] = (uint8_t)((hh ^ (hh >> 8) ^ (hh >> 16) ^ (hh >> 24)) & 0xFFu);
            bad[8] = 0xED; bad[9] = 0xFF; bad[10] = 0x0D;
            DC_CHECK(hw_dc_proto_parse(bad, 11) == HW_DCPP_ERR_CMD);
        }

        /* 参数错: 会话内 SET 但 payload 长度不足 (len=1 != 3) → ERR_ARG */
        hw_dc_init(NULL);
        fl = hw_dc_proto_build(DCPP_CMD_OPEN, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1);
        pl[0] = 0;
        fl = hw_dc_proto_build(DCPP_CMD_SET, 0, pl, 1, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_ARG);
        /* 参数错: GET 信号号越界 */
        pl[0] = 99;
        fl = hw_dc_proto_build(DCPP_CMD_GET, 0, pl, 1, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_ARG);
    }

    /* [16] 会话一条龙 + 计数 (OPEN..CLOSE); 关断后再命令 → ERR_CLOSED */
    hw_dc_init(NULL);
    {
        uint32_t rx = 0, tx = 0, err = 0;
        uint8_t last = 0;
        uint8_t fr[HW_DCPP_MAX_FRAME];
        int fl;

        /* PING(无需会话) → COUNT → OPEN → SET → GET → RANGE → DATA → BASE → CLOSE */
        fl = hw_dc_proto_build(DCPP_CMD_PING, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1 && hw_dc_proto_resp_value() == 0x50);
        fl = hw_dc_proto_build(DCPP_CMD_COUNT, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 2);
        fl = hw_dc_proto_build(DCPP_CMD_OPEN, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1 && hw_dc_proto_opened() == 1);
        {
            uint8_t pl[3]; pl[0] = 0; pl[1] = 42; pl[2] = 0;   /* SET sig0 = 42 */
            fl = hw_dc_proto_build(DCPP_CMD_SET, 0, pl, 3, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 1 && hw_dc_proto_resp_value() == 0);
            pl[0] = 0;                                          /* GET sig0 → 42 */
            fl = hw_dc_proto_build(DCPP_CMD_GET, 0, pl, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 2 && hw_dc_proto_resp_value() == 42);
            pl[0] = 0;                                          /* RANGE sig0 → 0 */
            fl = hw_dc_proto_build(DCPP_CMD_RANGE, 0, pl, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 1 && hw_dc_proto_resp_value() == 0);
            pl[0] = 4;                                          /* DATA 4 → 217 */
            fl = hw_dc_proto_build(DCPP_CMD_DATA, 0, pl, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 2 && hw_dc_proto_resp_value() == 0x0D9);
            pl[0] = (uint8_t)DC_SIDE_OUT;                       /* BASE OUT → 128 */
            fl = hw_dc_proto_build(DCPP_CMD_BASE, 0, pl, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == 2 && hw_dc_proto_resp_value() == 0x080);
        }
        fl = hw_dc_proto_build(DCPP_CMD_CLOSE, 0, NULL, 0, fr, (int)sizeof(fr));
        DC_CHECK(hw_dc_proto_parse(fr, fl) == 1 && hw_dc_proto_opened() == 0);

        hw_dc_proto_stats(&rx, &tx, &err, &last);
        DC_CHECK(rx == 9 && tx == 9 && err == 0 && last == (uint8_t)DCPP_CMD_CLOSE);

        /* 关断后 GET → ERR_CLOSED (err 计数 +1) */
        {
            uint8_t pl[1]; pl[0] = 0;
            fl = hw_dc_proto_build(DCPP_CMD_GET, 0, pl, 1, fr, (int)sizeof(fr));
            DC_CHECK(hw_dc_proto_parse(fr, fl) == HW_DCPP_ERR_CLOSED);
        }
        hw_dc_proto_stats(&rx, &tx, &err, &last);
        DC_CHECK(err == 1 && rx == 9);

        /* 命令通道打帧/成帧 (CLI/VM 共用通道判别性) */
        hw_dc_proto_reset();
        DC_CHECK(hw_dc_cmd("protook", NULL) == 1);
        DC_CHECK(hw_dc_cmd("protover", NULL) == (int)HW_DCPP_VER);
        DC_CHECK(hw_dc_cmd("protocount", NULL) == (int)DCPP_CMD_NUM);
        DC_CHECK(hw_dc_cmd("protobuild 1", NULL) == 11);       /* PING 帧长 */
        DC_CHECK(hw_dc_cmd("protobuild 3", NULL) == 12);       /* GET: 1B 负载 */
        DC_CHECK(hw_dc_cmd("protoframe 0", NULL) == 11);
        DC_CHECK(hw_dc_cmd("protoframe 16", NULL) == 27);
        DC_CHECK(hw_dc_cmd("protoframe 17", NULL) == -1);
        DC_CHECK(hw_dc_cmd("protocmd 8", NULL) == 1);          /* OPEN 成帧 → 会话开 */
        DC_CHECK(hw_dc_cmd("protoopened", NULL) == 1);
        DC_CHECK(hw_dc_cmd("protocmd 6", NULL) == 1);          /* RANGE */
        DC_CHECK(hw_dc_cmd("protocmd 9", NULL) == 1);          /* CLOSE */
        DC_CHECK(hw_dc_cmd("protoopened", NULL) == 0);
        DC_CHECK(hw_dc_cmd("protocmd 6", NULL) == HW_DCPP_ERR_CLOSED);
        DC_CHECK(hw_dc_cmd("protostat", NULL) == (int)HW_DCPP_IDLE);
        DC_CHECK(hw_dc_cmd("bogus", NULL) == -1);
        hw_dc_proto_reset();
    }

    /* 状态机闭环回环: 打帧→喂入→成帧, 计数正确 (端到端自证) */
    hw_dc_init(NULL);
    {
        uint32_t rx = 0, tx = 0, err = 0;
        uint8_t last = 0;
        uint8_t fr[HW_DCPP_MAX_FRAME];
        int i, r = 0, total = 0;
        /* 顺序: 先 PING/COUNT/OPEN 开会话, 再会话内命令, 最后 CLOSE */
        static const uint8_t seq[DCPP_CMD_NUM] = {
            DCPP_CMD_PING, DCPP_CMD_COUNT, DCPP_CMD_OPEN, DCPP_CMD_GET,
            DCPP_CMD_SET, DCPP_CMD_BASE, DCPP_CMD_RANGE, DCPP_CMD_DATA,
            DCPP_CMD_CLOSE
        };
        for (i = 0; i < (int)DCPP_CMD_NUM; i++) {
            int fl = dc_proto_build_cmd(seq[i], fr, (int)sizeof(fr));
            int k;
            DC_CHECK(fl > 0);
            for (k = 0; k < fl; k++) r = hw_dc_proto_feed(fr[k]);
            DC_CHECK(r == HW_DCPP_FEED_FRAME);
            total++;
        }
        hw_dc_proto_stats(&rx, &tx, &err, &last);
        DC_CHECK(rx == (uint32_t)total && tx == (uint32_t)total && err == 0);
        DC_CHECK(last == (uint8_t)DCPP_CMD_CLOSE);
    }
    hw_dc_init(NULL);

    if (fails == 0) {
        snprintf(line, sizeof(line),
                 "hw_dc selftest: all PASS (golden=0x%08X, mode=%s, sig=%u/data=%u)\n",
                 (unsigned)hw_dc_checksum(), hw_dc_mode_str(hw_dc_mode()),
                 (unsigned)DC_SIG_COUNT, (unsigned)DC_DATA_COUNT);
        if (putf) putf(line);   /* putf=NULL = 静默自检 (真机固件) */
    }
    return fails;
}

/* ============================================================
 * 7. CLI: ./xiaomo dc [card|sig N|set N V|base N|range N|data N|mode|selftest]
 * ============================================================ */
static int dc_cli_puts(const char* s) { return printf("%s", s); }

int hw_dc_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "card";

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_dc_selftest(dc_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "sig") == 0 && argc >= 4) {
        long sig = strtol(argv[3], NULL, 10);
        if (sig < 0 || sig >= (long)DC_SIG_COUNT) {
            printf("sig 越界 (0..%u)\n", (unsigned)(DC_SIG_COUNT - 1));
            return 1;
        }
        printf("%s = %d (%s)\n", hw_dc_sig_name((uint8_t)sig),
               hw_dc_sig_value((uint8_t)sig), hw_dc_sig_desc((uint8_t)sig));
        return 0;
    }
    if (strcmp(sub, "set") == 0 && argc >= 5) {
        long sig = strtol(argv[3], NULL, 10);
        long val = strtol(argv[4], NULL, 0);
        int rc = hw_dc_sig_set((uint8_t)sig, (int)val);
        printf("set %ld = %ld -> rc=%d\n", sig, val, rc);
        return (rc == 0) ? 0 : 1;
    }
    if (strcmp(sub, "base") == 0 && argc >= 4) {
        long side = strtol(argv[3], NULL, 10);
        int v = hw_dc_base_signal((uint8_t)side);
        if (v < 0) { printf("side 非法 (0=IN 1=OUT)\n"); return 1; }
        printf("base %s = 0x%03X\n", hw_dc_side_name((uint8_t)side), (unsigned)v);
        return 0;
    }
    if (strcmp(sub, "range") == 0 && argc >= 4) {
        long sig = strtol(argv[3], NULL, 10);
        int in = hw_dc_in_range((uint8_t)sig);
        printf("range %s = %d (%s)\n", hw_dc_sig_name((uint8_t)sig), in,
               in ? "在窗口内" : "越界");
        return 0;
    }
    if (strcmp(sub, "data") == 0 && argc >= 4) {
        long idx = strtol(argv[3], NULL, 10);
        printf("data %ld = 0x%03X\n", idx, (unsigned)hw_dc_data((uint8_t)idx));
        return 0;
    }
    if (strcmp(sub, "mode") == 0) {
        printf("mode = %s (%u)\n", hw_dc_mode_str(hw_dc_mode()),
               (unsigned)hw_dc_mode());
        return 0;
    }
    /* ---- DCPP 供电协议 (帧层) ---- */
    if (strcmp(sub, "proto") == 0) {
        const char* p = (argc >= 4) ? argv[3] : "card";

        if (strcmp(p, "reset") == 0) {
            hw_dc_proto_reset();
            printf("proto reset -> session closed, counters zero\n");
            return 0;
        }
        if (strcmp(p, "build") == 0 && argc >= 5) {
            uint8_t fr[HW_DCPP_MAX_FRAME];
            long c = strtol(argv[4], NULL, 10);
            int fl, i;
            if (c < 1 || c > (long)DCPP_CMD_NUM) {
                printf("build 命令非法 (1..%u)\n", (unsigned)DCPP_CMD_NUM);
                return 1;
            }
            fl = dc_proto_build_cmd((uint8_t)c, fr, (int)sizeof(fr));
            if (fl <= 0) { printf("build 失败\n"); return 1; }
            printf("build %-5s frame(%d) = ", hw_dc_proto_cmd_name((uint8_t)c), fl);
            for (i = 0; i < fl; i++) printf("%02X%s", fr[i], (i + 1 < fl) ? " " : "\n");
            return 0;
        }
        if (strcmp(p, "run") == 0 && argc >= 5) {
            long c = strtol(argv[4], NULL, 10);
            int r;
            if (c < 1 || c > (long)DCPP_CMD_NUM) {
                printf("run 命令非法 (1..%u)\n", (unsigned)DCPP_CMD_NUM);
                return 1;
            }
            r = dc_proto_run_cmd((uint8_t)c);
            printf("run %-5s rc=%d  opened=%d  state=%s  resp=%d\n",
                   hw_dc_proto_cmd_name((uint8_t)c), r, hw_dc_proto_opened(),
                   hw_dc_proto_state_name(hw_dc_proto_state()),
                   hw_dc_proto_resp_value());
            return (r == HW_DCPP_FEED_FRAME) ? 0 : 1;
        }
        if (strcmp(p, "loop") == 0) {     /* 全命令回环自证 */
            static const uint8_t seq[DCPP_CMD_NUM] = {
                DCPP_CMD_PING, DCPP_CMD_COUNT, DCPP_CMD_OPEN, DCPP_CMD_GET,
                DCPP_CMD_SET, DCPP_CMD_BASE, DCPP_CMD_RANGE, DCPP_CMD_DATA,
                DCPP_CMD_CLOSE
            };
            uint8_t fr[HW_DCPP_MAX_FRAME];
            uint32_t i, rx = 0, tx = 0, err = 0;
            uint8_t last = 0;
            int ok = 1;
            hw_dc_init(NULL);
            for (i = 0; i < DCPP_CMD_NUM; i++) {
                int fl = dc_proto_build_cmd(seq[i], fr, (int)sizeof(fr));
                int k, r = 0;
                if (fl <= 0) { ok = 0; break; }
                for (k = 0; k < fl; k++) r = hw_dc_proto_feed(fr[k]);
                if (r != HW_DCPP_FEED_FRAME) { ok = 0; break; }
            }
            hw_dc_proto_stats(&rx, &tx, &err, &last);
            printf("proto loop: %s  rx=%u tx=%u err=%u last=%s\n",
                   ok ? "ALL FRAMED" : "FAIL", (unsigned)rx, (unsigned)tx,
                   (unsigned)err, hw_dc_proto_cmd_name(last));
            return ok ? 0 : 1;
        }
        if (strcmp(p, "help") == 0) {
            printf("dc proto 子命令: card(协议卡) reset build N(打帧) "
                   "run N(喂帧) loop(全命令回环)\n");
            return 0;
        }
        /* ---- 协议卡 (默认) ---- */
        {
            uint32_t i;
            uint32_t rx = 0, tx = 0, err = 0;
            uint8_t last = 0;
            hw_dc_proto_stats(&rx, &tx, &err, &last);
            printf("=== xiaomo hw_dc DCPP 供电协议 (帧层) ===\n");
            printf("ver        = 0x%02X   cmds=%u   max_frame=%u\n",
                   (unsigned)hw_dc_proto_ver(), (unsigned)hw_dc_proto_cmd_num(),
                   (unsigned)HW_DCPP_MAX_FRAME);
            printf("proto cksum= 0x%08X %s\n", (unsigned)hw_dc_proto_checksum(),
                   (hw_dc_proto_checksum() == HW_DCPP_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
            printf("frame      = START(80 EF 02)|VER|SIDE|CMD|LEN|PAYLOAD|CKSUM8|END(ED FF 0D)\n");
            printf("session    = %s   state=%s\n",
                   hw_dc_proto_opened() ? "OPEN" : "CLOSED",
                   hw_dc_proto_state_name(hw_dc_proto_state()));
            printf("counters   = rx=%u tx=%u err=%u last=%s\n",
                   (unsigned)rx, (unsigned)tx, (unsigned)err,
                   hw_dc_proto_cmd_name(last));
            printf("---- 命令表 (count=%u) ----\n", (unsigned)DCPP_CMD_NUM);
            for (i = 0; i < DCPP_CMD_NUM; i++) {
                uint8_t fr[HW_DCPP_MAX_FRAME];
                int fl = dc_proto_build_cmd((uint8_t)(i + 1u), fr, (int)sizeof(fr));
                printf("  [0x%02X] %-6s frame=%2d  hex=", (unsigned)(i + 1u),
                       hw_dc_proto_cmd_name((uint8_t)(i + 1u)), fl);
                {
                    int k;
                    for (k = 0; k < fl; k++) printf("%02X", fr[k]);
                }
                printf("\n");
            }
            printf("VM 联动: .mo 端 hw_dc(\"protocmd 3\") / CLI ./xiaomo dc proto run 8\n");
            printf("  见 examples/dc_proto_test.mo\n");
            return 0;
        }
    }
    if (strcmp(sub, "help") == 0) {
        printf("dc 子命令: card(能力卡) sig N(取值) set N V(置值) base N(参考预值)\n");
        printf("  range N(窗口判决) data N(组合帧) mode selftest proto ...(供电协议)\n");
        return 0;
    }

    /* ---- 能力卡 (默认) ---- */
    {
        uint32_t ck = hw_dc_checksum();
        uint32_t i;
        hw_dc_init(NULL);
        printf("=== xiaomo hw_dc DC 电源信号层 ===\n");
        printf("mode       = %s (%u)\n", hw_dc_mode_str(hw_dc_mode()),
               (unsigned)hw_dc_mode());
        printf("sig cksum  = 0x%08X %s\n", (unsigned)ck,
               (ck == HW_DC_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
        printf("golden ok  = %d\n", hw_dc_cmd("ok", NULL));
        printf("---- 信号表 (count=%u) ----\n", (unsigned)DC_SIG_COUNT);
        for (i = 0; i < DC_SIG_COUNT; i++)
            printf("  [%u] %-6s side=%-3s val=%d  %s\n", (unsigned)i,
                   g_sig_table[i].name, hw_dc_side_name(g_sig_table[i].side),
                   hw_dc_sig_value((uint8_t)i), g_sig_table[i].desc);
        printf("---- 参考预值 / 组合数据帧 (count=%u) ----\n",
               (unsigned)DC_DATA_COUNT);
        printf("  base IN =0x%03X  base OUT=0x%03X\n",
               (unsigned)hw_dc_base_signal(DC_SIDE_IN),
               (unsigned)hw_dc_base_signal(DC_SIDE_OUT));
        for (i = 0; i < DC_DATA_COUNT; i++)
            printf("  DATA_%u = 0x%03X\n", (unsigned)(i + 1),
                   (unsigned)g_data_table[i]);
        printf("---- DCPP 供电协议 (帧层, count=%u) ----\n", (unsigned)DCPP_CMD_NUM);
        printf("  proto cksum = 0x%08X %s\n", (unsigned)hw_dc_proto_checksum(),
               (hw_dc_proto_checksum() == HW_DCPP_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
        printf("  frame  = START(80 EF 02)|VER|SIDE|CMD|LEN|PAYLOAD|CKSUM8|END(ED FF 0D)\n");
        printf("  cmds   = PING COUNT GET SET BASE RANGE DATA OPEN CLOSE\n");
        printf("  详见: ./xiaomo dc proto card | proto loop\n");
        printf("VM 内核联动: kvm_run 上电自动 hw_dc_init; .mo 端 hw_dc(\"...\")\n");
        printf("  见 examples/dc_test.mo\n");
        return 0;
    }
}
