/**
 * @file    hw_dmc.c
 * @brief   xiaomo hw_dmc — DMC 主从链路协议层 (全跨式, 2026-10-01)
 *
 * 整合自三份互不兼容的旧实现, 见 include/hw_dmc.h 顶部的整合说明。
 * 六模式 / BSP 注入 / 黄金值锁定 / 无 malloc / 无文件 IO / freestanding 可编译。
 */
#include "hw_dmc.h"
#include <string.h>
#include <stdio.h>    /* snprintf/vsnprintf —— freestanding 交叉编译时由 ESP-IDF 提供 */
#include <stdarg.h>

/* ============================================================
 * 六模式探测 (编译期宏优先)
 * ============================================================ */
#if defined(HW_DMC_MODE_OVERRIDE)
#  define DMC_MODE ((int)(HW_DMC_MODE_OVERRIDE))
#elif defined(HW_DMC_KELL)
#  define DMC_MODE HW_DMC_MODE_KELL
#elif defined(ESP8266) || defined(ESP8266_CORE_VERSION) || defined(__ets__)
#  define DMC_MODE HW_DMC_MODE_ESP8266
#elif defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3) || \
      defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || \
      defined(CONFIG_IDF_TARGET_ESP32H2) || defined(ESP_PLATFORM)
#  define DMC_MODE HW_DMC_MODE_ESP32
#elif defined(__linux__)
#  define DMC_MODE HW_DMC_MODE_LINUX
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
#  define DMC_MODE HW_DMC_MODE_HOST
#else
#  define DMC_MODE HW_DMC_MODE_TEST
#endif

static int g_mode = DMC_MODE;

int hw_dmc_mode(void) { return g_mode; }

const char* hw_dmc_mode_name(int mode)
{
    switch (mode) {
    case HW_DMC_MODE_HOST:    return "HOST";
    case HW_DMC_MODE_LINUX:   return "LINUX";
    case HW_DMC_MODE_KELL:    return "KELL";
    case HW_DMC_MODE_ESP32:   return "ESP32";
    case HW_DMC_MODE_ESP8266: return "ESP8266";
    case HW_DMC_MODE_TEST:    return "TEST";
    default:                  return "UNKNOWN";
    }
}

const char* hw_dmc_build_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "arm";
#elif defined(__riscv)
    return "riscv";
#elif defined(__xtensa__)
    /* 🕳️ 2026-10-01 补: ESP32-S3 交叉编译用的是 xtensa-esp32s3-elf-gcc,
     *   定义 __xtensa__。此前本函数**漏了这一支** → 真机上 hw_dmc_build_arch()
     *   一律返回 "unknown", 而宿主与 riscv 都正常, 属"只有真机才暴露"的坑。
     *   顺序放在 riscv 之后: 两组宏互斥, 但保持 riscv 在前不影响。 */
    return "xtensa";
#else
    return "unknown";
#endif
}

/* ============================================================
 * FNV-1a-32 / CRC16-CCITT (FALSE)
 * ⚠️ 家族纪律: 黄金值由独立 Python 锁定, 不许 C 自己算自己
 * ============================================================ */
uint32_t hw_dmc_fnv1a(const uint8_t* p, uint32_t n)
{
    uint32_t h = 0x811C9DC5U;
    uint32_t i;
    if (p == NULL) return 0;
    for (i = 0; i < n; i++) {
        h ^= (uint32_t)p[i];
        h *= 0x01000193U;
    }
    return h;
}

uint16_t hw_dmc_crc16(const uint8_t* data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t  j;
    if (data == NULL) return crc;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000U) crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ============================================================
 * 名字表 (与 Python 黄金表同源)
 * ============================================================ */
static const uint8_t g_cmd_tab[HW_DMC_CMD_NUM] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0F
};
static const char* const g_cmd_names[HW_DMC_CMD_NUM] = {
    "HELLO", "HELLO_ACK", "DATA", "DATA_ACK",
    "RESET", "RESET_ACK", "STATUS", "STATUS_RSP", "NACK"
};
static const char* const g_state_names[HW_DMC_STATE_NUM] = {
    "IDLE", "SEND_HELLO", "WAIT_HELLO_ACK", "SEND_RESET",
    "WAIT_RESET_ACK", "ESTABLISHED", "ERROR"
};
static const char* const g_err_names[HW_DMC_ERR_NUM] = {
    "OK", "TIMEOUT", "CRC", "FRAME", "STATE", "NACK", "BUSY", "PARAM"
};

const char* hw_dmc_cmd_name(uint8_t cmd)
{
    int i;
    for (i = 0; i < HW_DMC_CMD_NUM; i++)
        if (g_cmd_tab[i] == cmd) return g_cmd_names[i];
    return "UNKNOWN";
}

const char* hw_dmc_state_name(int st)
{
    if (st < 0 || st >= HW_DMC_STATE_NUM) return "INVALID";
    return g_state_names[st];
}

const char* hw_dmc_err_name(int e)
{
    if (e < 0 || e >= HW_DMC_ERR_NUM) return "INVALID";
    return g_err_names[e];
}

/* ============================================================
 * 黄金值合成 (对齐 Python: CMD_NUM×4 + STATE_FNV + FRAME_FNV + CRC_VEC_FNV)
 *
 * ⚠️ 状态名拼接**只有一处** = dmc_golden() 里的循环, 从 g_state_names[] 生成。
 *    曾在这里另存一份手抄的 g_state_join 常量 → 与状态表不同步 (少拼了
 *    WAIT_HELLO_ACK 的 "_ACK") 且长度需手数 (47 vs 68) = 单一真相源原则的
 *    反面教材。手抄副本一律删除, 需要拼接结果的场合调用 dmc_state_join_fnv()。
 * ============================================================ */

/* 状态名拼接的 FNV (唯一实现, 供 cmd 分发与自检共用) */
static uint32_t dmc_state_join_fnv(void);

/* 帧黄金向量 (与 Python 中 FRAMES 逐条同源) */
static uint8_t dmc_gold_frame(uint8_t cmd, const uint8_t* pl, uint16_t plen,
                              uint8_t* out)
{
    int n = hw_dmc_pack(cmd, pl, plen, out, HW_DMC_MAX_FRAME);
    return (n > 0) ? (uint8_t)n : 0U;
}

static uint32_t dmc_gold_frame_fnv(void)
{
    static uint8_t buf[64];
    static const uint8_t hello[8] = { 0x01, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t data2[4]  = { 0x00, 0x00, 'h', 'i' };
    uint32_t n = 0, a, b, c;
    a = dmc_gold_frame(HW_DMC_CMD_HELLO, hello, 8, buf);      n += a;
    b = dmc_gold_frame(HW_DMC_CMD_DATA,  data2,  4, buf + n); n += b;
    c = dmc_gold_frame(HW_DMC_CMD_RESET, NULL,  0, buf + n); n += c;
    if (a != 14 || b != 10 || c != 6) return 0U;   /* 长度锁, 与 Python 同 */
    return hw_dmc_fnv1a(buf, n);
}

static uint32_t dmc_state_join_fnv(void)
{
    /* ⚠️ 两处坑, 都是我自己写的:
     *   ① 缓冲要放得下拼接结果 (IDLE..ERROR 共 68 字节) → 128 留余量
     *   ② 守卫短路了 `*s++` → 满了以后指针不推进 → 死循环。
     *      正确写法: 先无条件推进指针, 再按需存入。 */
    static uint8_t blk[128];
    uint32_t i, state_len = 0;
    for (i = 0; i < (uint32_t)HW_DMC_STATE_NUM; i++) {
        const char* s = g_state_names[i];
        while (*s) {
            if (state_len < (uint32_t)sizeof(blk)) blk[state_len++] = (uint8_t)*s;
            s++;                       /* ⚠️ 无条件推进 (见上 ②) */
        }
    }
    return hw_dmc_fnv1a(blk, state_len);
}

static uint32_t dmc_golden(void)
{
    static uint8_t blk[16];
    static uint8_t crc_vec[9];
    uint32_t state_fnv, frame_fnv, crc_vec_fnv;
    uint32_t tail = 0;
    int d;

    state_fnv   = dmc_state_join_fnv();
    frame_fnv   = dmc_gold_frame_fnv();
    (void)hw_dmc_crc_vector(crc_vec, (int)sizeof(crc_vec));
    crc_vec_fnv = hw_dmc_fnv1a(crc_vec, 9);

    /* 前 4 字节 = CMD_NUM×4 作版本前缀; 后 12 字节 = 三个派生值小端
     * (与 Python bytes([9,9,9,9]) + struct.pack('<I')×3 同序) */
    for (d = 0; d < 4; d++) blk[tail++] = (uint8_t)HW_DMC_CMD_NUM;
    blk[tail++] = (uint8_t)(state_fnv & 0xFFU);
    blk[tail++] = (uint8_t)((state_fnv >> 8) & 0xFFU);
    blk[tail++] = (uint8_t)((state_fnv >> 16) & 0xFFU);
    blk[tail++] = (uint8_t)((state_fnv >> 24) & 0xFFU);
    blk[tail++] = (uint8_t)(frame_fnv & 0xFFU);
    blk[tail++] = (uint8_t)((frame_fnv >> 8) & 0xFFU);
    blk[tail++] = (uint8_t)((frame_fnv >> 16) & 0xFFU);
    blk[tail++] = (uint8_t)((frame_fnv >> 24) & 0xFFU);
    blk[tail++] = (uint8_t)(crc_vec_fnv & 0xFFU);
    blk[tail++] = (uint8_t)((crc_vec_fnv >> 8) & 0xFFU);
    blk[tail++] = (uint8_t)((crc_vec_fnv >> 16) & 0xFFU);
    blk[tail++] = (uint8_t)((crc_vec_fnv >> 24) & 0xFFU);

    return hw_dmc_fnv1a(blk, tail);
}

/* ============================================================
 * 成帧 / 解帧
 * LEN 覆盖 [sync0 .. crc末], CRC 覆盖 [len .. payload] (不含同步头)
 * ============================================================ */
int hw_dmc_pack(uint8_t cmd, const uint8_t* payload, uint16_t plen,
                uint8_t* out, int cap)
{
    uint32_t total;
    uint16_t crc;

    if (out == NULL)                       return HW_DMC_ERR_PARAM;
    if (plen > HW_DMC_MAX_PAYLOAD)         return HW_DMC_ERR_PARAM;
    if (plen > 0 && payload == NULL)       return HW_DMC_ERR_PARAM;
    total = (uint32_t)HW_DMC_HEADER_LEN + plen + HW_DMC_CRC_LEN;
    if (total > (uint32_t)HW_DMC_MAX_FRAME) return HW_DMC_ERR_PARAM;
    /* 定义性护栏: LEN 字段只 1 字节 → 整帧 > 255 无法表示 (会被静默截断成
     * uint8_t 而错位)。HW_DMC_MAX_PAYLOAD 已保证不触发, 这条是为了防止
     * 上界常量将来被改回 252 时缺陷重来 (历史上正是这么错了一整版)。 */
    if (total > 255U)                      return HW_DMC_ERR_PARAM;
    if (cap < (int)total)                  return HW_DMC_ERR_PARAM;

    out[0] = (uint8_t)HW_DMC_SYNC_0;
    out[1] = (uint8_t)HW_DMC_SYNC_1;
    out[2] = (uint8_t)total;
    out[3] = cmd;
    if (plen > 0) memcpy(&out[HW_DMC_HEADER_LEN], payload, plen);
    crc = hw_dmc_crc16(&out[2], (uint16_t)(total - 2 - HW_DMC_CRC_LEN));
    out[total - 2] = (uint8_t)(crc & 0xFFU);          /* 小端 */
    out[total - 1] = (uint8_t)((crc >> 8) & 0xFFU);
    return (int)total;
}

int hw_dmc_unpack(const uint8_t* in, int len, uint8_t* cmd_out,
                  const uint8_t** pl_out, uint16_t* plen_out)
{
    int      declared;
    uint16_t crc_recv, crc_calc;

    if (in == NULL || len < (int)HW_DMC_MIN_FRAME) return HW_DMC_ERR_FRAME;
    if (in[0] != (uint8_t)HW_DMC_SYNC_0)            return HW_DMC_ERR_FRAME;
    if (in[1] != (uint8_t)HW_DMC_SYNC_1)            return HW_DMC_ERR_FRAME;

    declared = (int)in[2];
    /* ⚠️ 修 bug: 原码先按 declared_len 索引 crc 再校验 len, 越界读。
     *    本版先校验 declared 落在 [MIN,MAX] 且 <= len, 再索引。 */
    if (declared < (int)HW_DMC_MIN_FRAME)           return HW_DMC_ERR_FRAME;
    if (declared > (int)HW_DMC_MAX_FRAME)           return HW_DMC_ERR_FRAME;
    if (len < declared)                             return HW_DMC_ERR_FRAME;

    crc_recv = (uint16_t)((uint16_t)in[declared - 2] |
                          ((uint16_t)in[declared - 1] << 8));
    crc_calc = hw_dmc_crc16(&in[2], (uint16_t)(declared - 2 - (int)HW_DMC_CRC_LEN));
    if (crc_recv != crc_calc)                       return HW_DMC_ERR_CRC;

    if (cmd_out) *cmd_out = in[3];
    if (pl_out)  *pl_out  = &in[HW_DMC_HEADER_LEN];
    if (plen_out)*plen_out= (uint16_t)(declared - (int)HW_DMC_HEADER_LEN - (int)HW_DMC_CRC_LEN);
    return HW_DMC_ERR_OK;
}

/* ============================================================
 * BSP + 默认实现 (确定性环回 + 确定性 tick)
 * ============================================================ */
static hw_dmc_bsp_t g_bsp;

/* 默认环回: 发送的字节进环形缓冲, 接收时取回 —— 主从互通自证。
 *
 * ⚠️ 结构性坑 (整合时踩到, 记录在案): 朴素环回下 master 发的帧会**被自己读回**,
 *    dmc_link_wait 期望 STATUS_RSP 却读到 STATUS → 返回 FRAME 而非 TIMEOUT,
 *    表现为「空链路查询报 FRAME」这种语义错乱的假象。
 *    真 UART 半双工**不会**回环自己的 TX, 所以模型必须复现这一点。
 *
 *    ⚠️ 第二版踩坑: 用「g_loop_tx_mark 递减计数」标记自己发的字节是错的 ——
 *      计数是**累计发送量**, 与「缓冲里前 N 个字节是自己发的」并不对应
 *      (尤其 loop_inject 从尾部追加时, 前缀语义被打乱, 表现为
 *      「ACK 明明收到了 rx=1, 但被判 TIMEOUT 且 txmark 被清零」)。
 *    正解 = **逐槽位打 owner 标记**, 取用时按标记判归属, 不用区间/计数。
 *
 * 🕳️🕳️ 这里连踩三次才做对, 值得完整记下 (家族坑 #18):
 *   ① 朴素环回: 把本端自己发的当应答读回 → 空链路查询返回 FRAME 而非 TIMEOUT
 *      (真 UART 是半双工, 绝不会回环自己 → 这是模型撒谎, 不是协议 bug)
 *   ② 递减计数 tx_mark: 「缓冲里还剩几个自己发的字节」
 *      → 错, 累计发送量 ≠ 缓冲前缀长度, 对端字节混进来就错
 *   ③ 单调上界 g_loop_tx_hi: 「自己发的占槽位 [0, hi)」
 *      → 仍错。**上界法隐含假设「tx 字节在缓冲里连续」, 但两个来源可以交错**:
 *        对端 ACK 先注入占槽 0..13, 本端 HELLO 后发占 14..27,
 *        发完 tx_hi 被推到 28 → 对端 ACK 全被判成「自己发的」丢弃 → 永远 TIMEOUT
 *   正解: 每个槽位一个 owner 字节 (OWN_TX / OWN_PEER), 逐槽位判。
 *   **教训: 当两个来源可以交错写入同一缓冲时, 用「区间/上界/计数」表达归属
 *      必然在某个交错顺序下失效 —— 归属必须逐元素存储。** */

#define DMC_LOOP_CAP 1024
/* 槽位归属标记: 0=空, 1=本端发出(半双工须跳过), 2=对端注入(要取用) */
#define DMC_OWN_EMPTY 0
#define DMC_OWN_TX    1
#define DMC_OWN_PEER  2
static uint8_t g_loop[DMC_LOOP_CAP];
static uint8_t g_loop_own[DMC_LOOP_CAP];
static int     g_loop_r = 0, g_loop_w = 0;
static uint32_t g_tick = 0;
/* 本端累计发出的字节数 (selftest 断言「帧真的发出去了」用)。
 * 独立计数器, 不从槽位推算 —— 槽位会绕回, 推不出累计量。 */
static int     g_loop_txbytes = 0;

static int dmc_def_tx(uint8_t ch)
{
    if (g_loop_w + 1 >= DMC_LOOP_CAP) return -1;
    g_loop[g_loop_w] = ch;
    g_loop_own[g_loop_w] = DMC_OWN_TX;
    g_loop_txbytes++;
    g_loop_w = (g_loop_w + 1) % DMC_LOOP_CAP;
    return 0;
}

static int dmc_def_rx(uint32_t timeout_ms)
{
    (void)timeout_ms;
    while (g_loop_r != g_loop_w) {
        int slot = g_loop_r;
        uint8_t ch = g_loop[slot];
        g_loop_r = (g_loop_r + 1) % DMC_LOOP_CAP;
        if (g_loop_own[slot] == DMC_OWN_TX) continue;  /* 半双工: 跳过本端自己发的槽位 */
        return (int)ch;
    }
    return -1;                          /* 空 → 超时 */
}

static int dmc_bsp_tx(uint8_t ch)
{
    if (g_bsp.tx) return g_bsp.tx(ch);
    return dmc_def_tx(ch);
}

static int dmc_bsp_rx(uint32_t to)
{
    if (g_bsp.rx) return g_bsp.rx(to);
    return dmc_def_rx(to);
}

static uint32_t dmc_bsp_now(void)
{
    if (g_bsp.now_ms) return g_bsp.now_ms();
    return g_tick;
}

void hw_dmc_bsp_install(const hw_dmc_bsp_t* bsp)
{
    if (bsp) { g_bsp = *bsp; }                    /* 拷成静态副本 (勿存栈指针) */
    else     { memset(&g_bsp, 0, sizeof(g_bsp)); } /* NULL = 卸载回默认环回 */
}

void hw_dmc_loop_reset(void)
{
    g_loop_r = 0; g_loop_w = 0; g_tick = 0; g_loop_txbytes = 0;
    memset(g_loop_own, DMC_OWN_EMPTY, sizeof(g_loop_own));
}

/* 注入「对端发来的字节」: 直接入环回缓冲且**不**打 tx 标记,
 * 用来在 selftest / 宿主侧模拟从设备 (master 侧协议对端)。
 * 真机链路上由真实 UART 硬件收, 不走这里。 */
int hw_dmc_loop_inject(const uint8_t* data, int len)
{
    int i;
    if (data == NULL || len <= 0) return HW_DMC_ERR_PARAM;
    for (i = 0; i < len; i++) {
        if (g_loop_w + 1 >= DMC_LOOP_CAP) return HW_DMC_ERR_BUSY;
        g_loop[g_loop_w] = data[i];
        g_loop_own[g_loop_w] = DMC_OWN_PEER;   /* 对端来的, rx 要取用 */
        g_loop_w = (g_loop_w + 1) % DMC_LOOP_CAP;
    }
    return HW_DMC_ERR_OK;
}

/* 取本端已发送的字节数 (selftest 断言用: 证明帧真的发出去了)。
 * 返回 = 本端累计发出的字节数。
 * ⚠️ 刻意不实现成「缓冲里还剩几个自己的字节」或「槽位上界」: 那两种表达
 *   在对端字节与本端字节交错时都不成立 (见上方坑 #18 三连)。 */
int hw_dmc_loop_txmark(void) { return g_loop_txbytes; }

/* ============================================================
 * 链路收发
 * ============================================================ */
void hw_dmc_link_init(hw_dmc_link_t* l, uint8_t local_addr, uint8_t slave_addr,
                      uint32_t timeout_ms)
{
    if (l == NULL) return;
    memset(l, 0, sizeof(*l));
    l->state      = HW_DMC_ST_IDLE;
    l->local_addr = local_addr;
    l->slave_addr = slave_addr;
    l->timeout_ms = timeout_ms;
}

int hw_dmc_link_state(const hw_dmc_link_t* l)
{
    return (l == NULL) ? HW_DMC_ERR_PARAM : l->state;
}

static int dmc_link_send(hw_dmc_link_t* l, uint8_t cmd,
                         const uint8_t* pl, uint16_t plen)
{
    static uint8_t buf[HW_DMC_MAX_FRAME];    /* 静态化: 真机栈安全 (坑 #12) */
    int n, i;
    if (l == NULL) return HW_DMC_ERR_PARAM;
    n = hw_dmc_pack(cmd, pl, plen, buf, (int)sizeof(buf));
    if (n < 0) return n;
    for (i = 0; i < n; i++) {
        if (dmc_bsp_tx(buf[i]) < 0) { l->state = HW_DMC_ST_ERROR; return HW_DMC_ERR_BUSY; }
    }
    l->tx_count++;
    l->last_tx_cmd = cmd;
    return HW_DMC_ERR_OK;
}

/* 等一帧: 逐字节收, 按协议头解析长度 (不假设一次读满) */
static int dmc_link_wait(hw_dmc_link_t* l, uint8_t expect, uint8_t* pl_out,
                         int cap, uint16_t* plen_out, uint32_t timeout_ms)
{
    static uint8_t buf[HW_DMC_MAX_FRAME];    /* 静态化: 真机栈安全 (坑 #12) */
    uint32_t start = dmc_bsp_now();
    int      n = 0;
    uint8_t  cmd = 0;
    const uint8_t* pl = NULL;
    uint16_t  pl_len = 0;
    int      r, u;

    if (l == NULL) return HW_DMC_ERR_PARAM;

    while (n < (int)HW_DMC_MAX_FRAME) {
        /* 已有足够字节则尝试解帧 */
        if (n >= (int)HW_DMC_MIN_FRAME) {
            if (buf[0] == (uint8_t)HW_DMC_SYNC_0 && buf[1] == (uint8_t)HW_DMC_SYNC_1) {
                u = hw_dmc_unpack(buf, n, &cmd, &pl, &pl_len);
                if (u == HW_DMC_ERR_OK) {
                    l->rx_count++;
                    l->last_rx_cmd = cmd;
                    if (expect != 0 && cmd != expect) {
                        if (cmd == HW_DMC_CMD_NACK) return HW_DMC_ERR_NACK;
                        return HW_DMC_ERR_FRAME;
                    }
                    if (pl_out != NULL && pl_len > 0) {
                        if (cap < (int)pl_len) return HW_DMC_ERR_PARAM;
                        memcpy(pl_out, pl, pl_len);
                    }
                    if (plen_out) *plen_out = pl_len;
                    return HW_DMC_ERR_OK;
                }
                if (u == HW_DMC_ERR_CRC && n >= (int)buf[2]) {
                    l->err_crc++;
                    return HW_DMC_ERR_CRC;
                }
            } else {
                /* 同步头失配 → 右移 1 字节重同步 (逐字节流残留处理, 坑 #20) */
                int i;
                for (i = 0; i + 1 < n; i++) buf[i] = buf[i + 1];
                n--;
                continue;
            }
        }
        r = dmc_bsp_rx(timeout_ms);
        if (r < 0) { l->err_timeout++; return HW_DMC_ERR_TIMEOUT; }
        buf[n++] = (uint8_t)r;
        if (dmc_bsp_now() - start > timeout_ms) { l->err_timeout++; return HW_DMC_ERR_TIMEOUT; }
    }
    l->err_frame++;
    return HW_DMC_ERR_FRAME;
}

int hw_dmc_handshake_master(hw_dmc_link_t* l, int max_retries)
{
    static uint8_t hello[8] = { 0x00, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    static uint8_t ack[16];
    uint16_t alen = 0;
    int r;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    if (max_retries < 1) max_retries = 1;

    hello[0] = l->local_addr;      /* 静态数组就地填本机地址 (真机栈安全) */
    l->state = HW_DMC_ST_SEND_HELLO;

    for (l->retries = 0; (int)l->retries < max_retries; l->retries++) {
        r = dmc_link_send(l, HW_DMC_CMD_HELLO, hello, 8);
        if (r != HW_DMC_ERR_OK) continue;
        l->state = HW_DMC_ST_WAIT_HELLO_ACK;
        r = dmc_link_wait(l, HW_DMC_CMD_HELLO_ACK, ack, (int)sizeof(ack),
                          &alen, l->timeout_ms);
        if (r == HW_DMC_ERR_OK) {
            l->state = HW_DMC_ST_ESTABLISHED;
            l->seq_tx = 0; l->seq_rx = 0;
            return HW_DMC_ERR_OK;
        }
    }
    l->state = HW_DMC_ST_ERROR;
    return HW_DMC_ERR_TIMEOUT;
}

int hw_dmc_handshake_slave(hw_dmc_link_t* l, uint32_t wait_timeout_ms)
{
    static uint8_t hello[16], ack[8];
    uint16_t hlen = 0;
    int r;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    l->state = HW_DMC_ST_WAIT_HELLO_ACK;

    r = dmc_link_wait(l, HW_DMC_CMD_HELLO, hello, (int)sizeof(hello),
                      &hlen, wait_timeout_ms);
    if (r != HW_DMC_ERR_OK) { l->state = HW_DMC_ST_ERROR; return r; }

    ack[0] = l->local_addr; ack[1] = 0x01; ack[2] = 0xFF; ack[3] = 0xFF;
    ack[4] = 0x00; ack[5] = 0; ack[6] = 0; ack[7] = 0;
    r = dmc_link_send(l, HW_DMC_CMD_HELLO_ACK, ack, 8);
    l->state = (r == HW_DMC_ERR_OK) ? HW_DMC_ST_ESTABLISHED : HW_DMC_ST_ERROR;
    return r;
}

int hw_dmc_send_data(hw_dmc_link_t* l, const uint8_t* data, uint16_t len,
                     int max_retries)
{
    static uint8_t payload[HW_DMC_MAX_PAYLOAD];   /* 静态化: 真机栈安全 */
    static uint8_t ack[8];
    uint16_t alen = 0;
    int r, i;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    if (l->state != HW_DMC_ST_ESTABLISHED) return HW_DMC_ERR_STATE;
    if (data == NULL && len > 0) return HW_DMC_ERR_PARAM;
    /* ⚠️ 上界检查: 前 2 字节是 seq, DATA 最大可用负载 = 250 */
    if (len > (uint16_t)(HW_DMC_MAX_PAYLOAD - 2)) return HW_DMC_ERR_PARAM;
    if (max_retries < 1) max_retries = 1;

    payload[0] = (uint8_t)(l->seq_tx & 0xFFU);
    payload[1] = (uint8_t)((l->seq_tx >> 8) & 0xFFU);
    if (len > 0) memcpy(&payload[2], data, len);

    for (i = 0; i < max_retries; i++) {
        r = dmc_link_send(l, HW_DMC_CMD_DATA, payload, (uint16_t)(len + 2));
        if (r != HW_DMC_ERR_OK) continue;
        r = dmc_link_wait(l, HW_DMC_CMD_DATA_ACK, ack, (int)sizeof(ack),
                          &alen, l->timeout_ms);
        if (r == HW_DMC_ERR_OK) {
            if (alen >= 2 && ack[0] == payload[0] && ack[1] == payload[1]) {
                l->seq_tx++;
                return HW_DMC_ERR_OK;
            }
        }
    }
    return HW_DMC_ERR_TIMEOUT;
}

int hw_dmc_recv_data(hw_dmc_link_t* l, uint8_t* out, int cap, uint16_t* recv_len,
                     uint32_t timeout_ms)
{
    static uint8_t ack[2];
    static uint8_t raw[HW_DMC_MAX_PAYLOAD];
    uint16_t raw_len = 0, alen = 0;
    int r;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    if (l->state != HW_DMC_ST_ESTABLISHED) return HW_DMC_ERR_STATE;
    if (out == NULL && cap > 0) return HW_DMC_ERR_PARAM;

    /* ⚠️ 契约修复 (整合时发现): 原实现直接把整段 payload (含 2 字节 seq)
     *   拷进 out, 却把 *recv_len 报成 payload_len - 2。调用方按
     *   *recv_len 去读 out[0..] 就会读到 seq 而非业务数据 —— 长度与内容
     *   错位, 是最难查的一类 bug。本版: 收进 raw, 剥掉 seq 再拷给调用方,
     *   *recv_len 与 out 的内容长度**始终一致**。 */
    r = dmc_link_wait(l, HW_DMC_CMD_DATA, raw, (int)sizeof(raw), &raw_len, timeout_ms);
    if (r != HW_DMC_ERR_OK) return r;
    if (raw_len < 2) { dmc_link_send(l, HW_DMC_CMD_NACK, NULL, 0); return HW_DMC_ERR_FRAME; }

    ack[0] = (uint8_t)(l->seq_rx & 0xFFU);
    ack[1] = (uint8_t)((l->seq_rx >> 8) & 0xFFU);
    dmc_link_send(l, HW_DMC_CMD_DATA_ACK, ack, 2);

    if (out != NULL && (int)(raw_len - 2) > 0)
        memcpy(out, &raw[2], (size_t)(raw_len - 2));
    if (recv_len) *recv_len = (uint16_t)(raw_len - 2);   /* 与 out 内容一致 */
    l->seq_rx++;
    (void)alen;
    return HW_DMC_ERR_OK;
}

int hw_dmc_reset_link(hw_dmc_link_t* l, int max_retries)
{
    static uint8_t ack[8];
    uint16_t alen = 0;
    int r, i;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    if (max_retries < 1) max_retries = 1;
    l->state = HW_DMC_ST_SEND_RESET;

    for (i = 0; i < max_retries; i++) {
        r = dmc_link_send(l, HW_DMC_CMD_RESET, NULL, 0);
        if (r != HW_DMC_ERR_OK) continue;
        l->state = HW_DMC_ST_WAIT_RESET_ACK;
        r = dmc_link_wait(l, HW_DMC_CMD_RESET_ACK, ack, (int)sizeof(ack),
                          &alen, l->timeout_ms);
        if (r == HW_DMC_ERR_OK) {
            l->seq_tx = 0; l->seq_rx = 0;
            l->state = HW_DMC_ST_IDLE;
            return HW_DMC_ERR_OK;
        }
    }
    l->state = HW_DMC_ST_ERROR;
    return HW_DMC_ERR_TIMEOUT;
}

int hw_dmc_query_status(hw_dmc_link_t* l, uint8_t* status_byte, uint32_t timeout_ms)
{
    static uint8_t rsp[8];
    uint16_t rlen = 0;
    int r;
    if (l == NULL) return HW_DMC_ERR_PARAM;
    r = dmc_link_send(l, HW_DMC_CMD_STATUS, NULL, 0);
    if (r != HW_DMC_ERR_OK) return r;
    r = dmc_link_wait(l, HW_DMC_CMD_STATUS_RSP, rsp, (int)sizeof(rsp), &rlen, timeout_ms);
    if (r == HW_DMC_ERR_OK && status_byte != NULL && rlen >= 1)
        *status_byte = rsp[0];
    return r;
}

void hw_dmc_stats(const hw_dmc_link_t* l, uint32_t* tx, uint32_t* rx,
                  uint32_t* crc_err, uint32_t* to_err)
{
    if (l == NULL) return;
    if (tx)      *tx      = l->tx_count;
    if (rx)      *rx      = l->rx_count;
    if (crc_err) *crc_err = l->err_crc;
    if (to_err)  *to_err  = l->err_timeout;
}

/* ============================================================
 * 草稿 API 意图层 (2026-10-01 提取并实现)
 *   生命周期: porto_init → proto_create → proto_ready → run_cmd
 *   全部薄封装既有原语, 不复制协议逻辑。
 * ============================================================ */

int hw_dmc_porto_init(const hw_dmc_bsp_t* bsp)
{
    /* 草稿语义 = 装底层通道。bsp==NULL 走卸载回默认环回(与既有语义一致)。 */
    hw_dmc_bsp_install(bsp);
    return HW_DMC_ERR_OK;
}

int hw_dmc_proto_create(hw_dmc_link_t* l, uint8_t local_addr,
                        uint8_t slave_addr, uint32_t timeout_ms)
{
    if (l == NULL) return HW_DMC_ERR_PARAM;
    hw_dmc_link_init(l, local_addr, slave_addr, timeout_ms);
    return HW_DMC_ERR_OK;
}

int hw_dmc_proto_ready(const hw_dmc_link_t* l)
{
    if (l == NULL) return HW_DMC_ERR_PARAM;
    /* 只有握手完成才是 ready; 其余一律 STATE 错, 调用方不可误当可用。 */
    return (l->state == HW_DMC_ST_ESTABLISHED) ? HW_DMC_ERR_OK : HW_DMC_ERR_STATE;
}

int hw_dmc_proto_run_cmd(hw_dmc_link_t* l, uint8_t cmd,
                         const uint8_t* payload, uint16_t plen,
                         uint8_t* resp, int resp_cap, int* resp_len,
                         uint32_t timeout_ms)
{
    int r;

    if (l == NULL) return HW_DMC_ERR_PARAM;
    if (payload == NULL && plen > 0) return HW_DMC_ERR_PARAM;
    /* 响应缓冲的声明容量不得为负, 也不得声称比帧上界还大 */
    if (resp_cap < 0 || resp_cap > (int)HW_DMC_MAX_PAYLOAD) return HW_DMC_ERR_PARAM;
    if (resp_len) *resp_len = 0;

    /* DATA 走带 seq 前缀 + ACK 校验的专用路径, 不重复实现 */
    if (cmd == HW_DMC_CMD_DATA) {
        if (resp && resp_cap > 0) {
            uint16_t alen = 0;
            static uint8_t ack[8];
            r = dmc_link_wait(l, HW_DMC_CMD_DATA_ACK, ack, (int)sizeof(ack),
                              &alen, timeout_ms);
            if (r == HW_DMC_ERR_OK) {
                int n = (int)((alen < (uint16_t)resp_cap) ? alen : (uint16_t)resp_cap);
                memcpy(resp, ack, (size_t)n);
                if (resp_len) *resp_len = n;
            }
            return r;
        }
        return hw_dmc_send_data(l, payload, plen, 1);
    }

    /* 其余命令: 发出去后等对端回帧(不预判 ACK 类型) */
    r = dmc_link_send(l, cmd, payload, plen);
    if (r != HW_DMC_ERR_OK) return r;

    if (resp != NULL && resp_cap > 0) {
        uint16_t rlen = 0;
        r = dmc_link_wait(l, 0 /* 任意 cmd */, resp, resp_cap, &rlen, timeout_ms);
        /* 🕳️ dmc_link_wait 只在 `expect != 0` 分支里识别 NACK; 这里传 0 等
         *    "任意命令", 对端的 NACK 会被当正常应答吞掉。故在此显式补判,
         *    否则「被拒绝」会被上报成「成功」—— 协议对、调用方错。 */
        if (r == HW_DMC_ERR_OK && l->last_rx_cmd == HW_DMC_CMD_NACK)
            return HW_DMC_ERR_NACK;
        if (r == HW_DMC_ERR_OK && resp_len) *resp_len = (int)rlen;
    }
    return r;
}

/* ============================================================
 * init / hook
 * ============================================================ */
void hw_dmc_init(void* arg)
{
    (void)arg;
    hw_dmc_loop_reset();
    /* ⚠️ 有意「不」清 g_bsp: BSP = 设备级 UART 绑定, 属「环境」
     *    而非本模块内部状态。上电复位只清环回缓冲; 卸载只走
     *    hw_dmc_bsp_install(NULL)。真机陷阱 (坑 #25): kvm_run 上电
     *    自动调 init, 若此处抹 BSP 则真机 UART 绑定被悄悄清空。 */
}

void hw_dmc_hook(void* arg) { (void)arg; }

/* ============================================================
 * 命令分发
 * ============================================================ */
int hw_dmc_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (cmd == NULL) return -1;
    if (strcmp(cmd, "count") == 0)   return HW_DMC_CMD_NUM;
    if (strcmp(cmd, "states") == 0)  return HW_DMC_STATE_NUM;
    if (strcmp(cmd, "errs") == 0)    return HW_DMC_ERR_NUM;
    if (strcmp(cmd, "maxpay") == 0)  return (int)HW_DMC_MAX_PAYLOAD;
    if (strcmp(cmd, "maxframe") == 0)return (int)HW_DMC_MAX_FRAME;
    if (strcmp(cmd, "minframe") == 0)return (int)HW_DMC_MIN_FRAME;
    if (strcmp(cmd, "mode") == 0)    return DMC_MODE;
    /* ---- 以下三个是 [15b] 用例逼出来的补齐: usage 承诺过, 但
     *      hw_dmc_cmd 出口从未实现 → .mo 端 hw_dmc("cmds") 会拿到 -1。
     *      教训: 帮助文本是**契约**, 不是注释。 ---- */
    if (strcmp(cmd, "cmds") == 0)     return HW_DMC_CMD_NUM;
    if (strcmp(cmd, "frame") == 0)    return HW_DMC_MAX_FRAME;   /* 详细布局见 CLI 分支 */
    if (strcmp(cmd, "crcvec") == 0)   return (int)(hw_dmc_crc_of_vector() == HW_DMC_CRC_VECTOR) ? 1 : 0;
    if (strcmp(cmd, "hello") == 0) {
        static const uint8_t h8[8] = { 0x01, 0x01, 0xFF, 0xFF, 0, 0, 0, 0 };
        static uint8_t fb[HW_DMC_MAX_FRAME];
        const uint8_t* pl = NULL;      /* unpack 出参指向输入缓冲内部, 不拷贝 */
        uint16_t pl_len = 0; uint8_t gc = 0;
        int fn = hw_dmc_pack(HW_DMC_CMD_HELLO, h8, 8, fb, (int)sizeof(fb));
        int ur = hw_dmc_unpack(fb, fn, &gc, &pl, &pl_len);
        return (ur == 0 && gc == HW_DMC_CMD_HELLO && pl_len == 8 &&
                memcmp(pl, h8, 8) == 0) ? 1 : 0;
    }
    if (strcmp(cmd, "golden") == 0)  return (int)dmc_golden();
    if (strcmp(cmd, "cmdfnv") == 0)  return (int)hw_dmc_fnv1a(g_cmd_tab, HW_DMC_CMD_NUM);
    if (strcmp(cmd, "statefnv") == 0)return (int)dmc_state_join_fnv();
    if (strcmp(cmd, "framefnv") == 0) return (int)dmc_gold_frame_fnv();
    if (strcmp(cmd, "ok") == 0)      return (dmc_golden() == HW_DMC_GOLDEN) ? 1 : 0;
    if (strcmp(cmd, "crclen") == 0)  return (int)strlen("123456789");
    if (strcmp(cmd, "help") == 0)    return -2;

    /* ---- 带数值参数: "前缀 + 十进制" (如 "frame 7") ----
     * ⚠️ 为什么这里需要: mo2kbc 双参内置 hw_dmc("fmt", 数值) 编译成
     *   OP_HW_DMC_CALL 的 imm=R_TMP+1, VM 侧 (vm_core.c) 把寄存器值
     *   十进制**拼到命令串尾**再调进来 (见 case OP_HW_DMC_CALL)。
     *   若此处只做 strcmp 精确匹配, 双参路径**永远返回 -1** ——
     *   那是一条看起来实现了、实际全线报错的死路径。
     * 🕳️ 坑: 初版我把"数值解析"写进了头文件注释 ("双参: frame <cmd> <plen>")
     *   却没写实现, 自己照注释写 .mo 全拿 -1。**注释不是契约, 断言才是。 */
    {
        const char* p = cmd;
        while (*p && *p != ' ') p++;         /* 扫到首个空格 = 数值起点 */
        if (*p == ' ') {
            char name[32];
            size_t n = (size_t)(p - cmd);
            const char* q = p + 1;
            long val = 0;
            int ndig = 0;
            if (n == 0 || n >= sizeof(name)) return -1;   /* 越界防溢出 */
            memcpy(name, cmd, n);
            name[n] = '\0';
            /* 🕳️ 刻意**不用** strtol + endp:
             *   ① freestanding 交叉编译时 strtol 可能退化为返回 0 的桩,
             *      endp 不被赋值 → 误判"非数字", 宿主却全绿 (真机才炸);
             *   ② 家族内 hw_dc.c 用的都是 strtol(.., NULL, 10),
             *      从来没人用过 endp 这条路径, 无先例可循。
             *   自写解析零依赖、行为确定, 且两个形态必然一致。 */
            while (*q == ' ') q++;                          /* 允许多空格 */
            {   int neg = 0;
                if (*q == '-') { q++; if (*q == '-') return -1; neg = 1; }
                while (*q >= '0' && *q <= '9') {
                    val = val * 10 + (*q - '0');
                    if (val > 1000000L) return -1;          /* 防溢出 */
                    q++; ndig++;
                }
                if (neg) val = -val;                        /* 🕳️ 必须在这里取负:
                                                          *   漏了它 val=0 起算
                                                          *   → "num -5" 返回 5
                                                          *   (正负号被吞)。
                                                          *   由 [15c] 抓出。 */
            }
            if (ndig == 0) return -1;                       /* 无数字 → 不认 */
            while (*q == ' ') q++;
            if (*q != '\0') return -1;                      /* 尾随垃圾 → 不认 */
            /* 数值查询: 把常量与传入值做算术, 供 .mo 端验证双参路径真的
             * 参与了运算 (而不是被静默忽略)。
             * 未知前缀一律 -1 —— 不静默吞掉数值, 那会让拼错的命令
             * 悄悄返回"看起来合理"的结果。 */
            if (strcmp(name, "num") == 0) return (int)val;
            if (strcmp(name, "add") == 0) return (int)(val + HW_DMC_CMD_NUM);
            if (strcmp(name, "sub") == 0) return (int)(val - HW_DMC_CMD_NUM);
            if (strcmp(name, "mul") == 0) return (int)(val * HW_DMC_MAX_PAYLOAD);
            return -1;
        }
    }
    return -1;
}

/* CRC 向量查询 (CLI/示例用): 写 out, 返回长度 */
int hw_dmc_crc_vector(uint8_t* out, int cap)
{
    static const char* v = "123456789";
    int n = 9, i;
    if (out == NULL || cap < 2) return 0;
    for (i = 0; i < n; i++) out[i] = (uint8_t)v[i];
    return n;
}

uint16_t hw_dmc_crc_of_vector(void)
{
    static uint8_t v[9];
    int n = hw_dmc_crc_vector(v, (int)sizeof(v));
    return hw_dmc_crc16(v, (uint16_t)n);
}

/* ============================================================
 * selftest  (putf=NULL 时静默只返回结果码)
 * ============================================================ */
static int dmc_t_put(int (*putf)(const char*), const char* fmt, ...)
{
    char line[192];
    va_list ap;
    if (putf == NULL) return 0;   /* 静默模式: 只返回结果码 */
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return putf(line);
}

int hw_dmc_selftest(int (*putf)(const char*))
{
    static hw_dmc_link_t lk;            /* 静态化: 真机栈安全 (坑 #12) */
    static uint8_t buf[HW_DMC_MAX_FRAME];
    static uint8_t back[HW_DMC_MAX_FRAME];
    static uint8_t dmc_t_scratch[64];
    static const uint8_t hello[8] = { 0x01, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    static hw_dmc_bsp_t saved_bsp;      /* 结尾恢复, 不污染真机扫描 (坑 #25) */
    const uint8_t* pl = NULL;
    uint16_t pl_len = 0;
    uint8_t  cmd = 0, status = 0;
    int fails = 0, n, i, r, c;

    saved_bsp = g_bsp;                  /* [0] 保护真机 BSP */
    hw_dmc_loop_reset();

    /* ---- [1] 六模式 ---- */
    dmc_t_put(putf, "[1] mode=%s arch=%s expect=%s\n",
              hw_dmc_mode_name(DMC_MODE), hw_dmc_build_arch(),
              hw_dmc_mode_name(DMC_MODE));
    c = (DMC_MODE >= 0 && DMC_MODE < HW_DMC_MODE_MAX);
    if (!c) { fails++; dmc_t_put(putf, "  FAIL mode range\n", 0, -1); }

    /* ---- [2] CRC16 国际标准向量 ("123456789" -> 0x29B1) ---- */
    r = (int)hw_dmc_crc_of_vector();
    dmc_t_put(putf, "[2] crc(\"123456789\")=0x%04X expect=0x%04X\n", r, (int)HW_DMC_CRC_VECTOR);
    if (r != (int)HW_DMC_CRC_VECTOR) { fails++; dmc_t_put(putf, "  FAIL crc vector\n", 0, -1); }

    /* ---- [3] CRC 边界: 空串 0xFFFF / "A" 0xB915 ---- */
    r = (int)hw_dmc_crc16((const uint8_t*)"", 0);
    i = (int)hw_dmc_crc16((const uint8_t*)"A", 1);
    dmc_t_put(putf, "[3] crc empty=0x%04X 'A'=0x%04X expect=0xFFFF/0xB915\n", r, i);
    if (r != 0xFFFF || i != 0xB915) { fails++; dmc_t_put(putf, "  FAIL crc bounds\n", 0, -1); }

    /* ---- [4] 命令表 9 条 + FNV ---- */
    c = (int)hw_dmc_fnv1a(g_cmd_tab, HW_DMC_CMD_NUM);
    dmc_t_put(putf, "[4] cmds=%d fnv=0x%08X expect=0x%08X\n",
              HW_DMC_CMD_NUM, c, (int)HW_DMC_CMD_FNV);
    if (c != (int)HW_DMC_CMD_FNV) { fails++; dmc_t_put(putf, "  FAIL cmd table\n", 0, -1); }

    /* ---- [5] 状态表 7 条 ---- */
    c = HW_DMC_STATE_NUM;
    dmc_t_put(putf, "[5] states=%d expect=7 errs=%d\n", c, HW_DMC_ERR_NUM);
    if (c != 7 || HW_DMC_ERR_NUM != 8) { fails++; dmc_t_put(putf, "  FAIL state/err count\n", 0, -1); }

    /* ---- [6] 帧黄金向量: HELLO 8B -> 14 字节 AA550E01... ---- */
    n = hw_dmc_pack(HW_DMC_CMD_HELLO, hello, 8, buf, (int)sizeof(buf));
    dmc_t_put(putf, "[6] pack HELLO len=%d expect=14 head=%02X%02X%02X%02X\n",
              n, buf[0], buf[1], buf[2], buf[3]);
    if (n != 14 || buf[0] != 0xAA || buf[1] != 0x55 || buf[2] != 14 || buf[3] != 0x01) {
        fails++; dmc_t_put(putf, "  FAIL pack hello\n", 0, -1);
    }

    /* ---- [7] pack->unpack 往返逐位一致 ---- */
    cmd = 0; pl = NULL; pl_len = 0;
    r = hw_dmc_unpack(buf, n, &cmd, &pl, &pl_len);
    c = (r == HW_DMC_ERR_OK && cmd == HW_DMC_CMD_HELLO && pl_len == 8 &&
         memcmp(pl, hello, 8) == 0);
    dmc_t_put(putf, "[7] unpack rc=%d cmd=%s plen=%d roundtrip=%d\n",
              r, hw_dmc_cmd_name(cmd), (int)pl_len, c);
    if (!c) { fails++; dmc_t_put(putf, "  FAIL roundtrip\n", 0, -1); }

    /* ---- [8] 帧黄金 FNV 0xB3DFA0D8 ---- */
    c = (int)dmc_gold_frame_fnv();
    dmc_t_put(putf, "[8] framefnv=0x%08X expect=0x%08X\n", c, (int)HW_DMC_FRAME_FNV);
    if (c != (int)HW_DMC_FRAME_FNV) { fails++; dmc_t_put(putf, "  FAIL frame fnv\n", 0, -1); }

    /* ---- [9] 总黄金 0x3DBC1419 ---- */
    c = (int)dmc_golden();
    dmc_t_put(putf, "[9] golden=0x%08X expect=0x%08X\n", c, (int)HW_DMC_GOLDEN);
    if (c != (int)HW_DMC_GOLDEN) { fails++; dmc_t_put(putf, "  FAIL golden\n", 0, -1); }

    /* ---- [10] 负向: 翻 CRC 必须被拒 (取首个负码, 坑 #24) ---- */
    memcpy(back, buf, (size_t)n);
    back[n - 1] ^= 0x01;
    r = hw_dmc_unpack(back, n, &cmd, &pl, &pl_len);
    dmc_t_put(putf, "[10] bad crc rc=%d (%s) expect=%d (CRC)\n",
              r, hw_dmc_err_name(r), (int)HW_DMC_ERR_CRC);
    if (r != HW_DMC_ERR_CRC) { fails++; dmc_t_put(putf, "  FAIL bad crc\n", 0, -1); }

    /* ---- [11] 负向: 坏同步头 ---- */
    memcpy(back, buf, (size_t)n);
    back[0] = 0x00;
    r = hw_dmc_unpack(back, n, &cmd, &pl, &pl_len);
    dmc_t_put(putf, "[11] bad sync rc=%d expect=%d (FRAME)\n", r, (int)HW_DMC_ERR_FRAME);
    if (r != HW_DMC_ERR_FRAME) { fails++; dmc_t_put(putf, "  FAIL bad sync\n", 0, -1); }

    /* ---- [12] 负向: 声明长度越界 (修掉的越界读 bug 回归护栏) ---- */
    memcpy(back, buf, (size_t)n);
    back[2] = 0xFF;   /* declared=255 = 1B 能表示的最大值(合法上界), 但 > n=14 → 拒 */
    r = hw_dmc_unpack(back, n, &cmd, &pl, &pl_len);
    dmc_t_put(putf, "[12] declared>n rc=%d expect=%d (FRAME, 先校验后索引)\n",
              r, (int)HW_DMC_ERR_FRAME);
    if (r != HW_DMC_ERR_FRAME) { fails++; dmc_t_put(putf, "  FAIL declared bound\n", 0, -1); }

    /* ---- [13] 上界: payload MAX 放得下且**能解回**, MAX+1 拒绝 ----
     * 🕳️ 旧版只断言 pack 的返回长度(=258) → LEN 字段(uint8_t)回绕成 2
     *    被完整遮住: 帧「打包成功」却结构性无法解出, 真链路还会错位。
     *    上界用例必须 **round-trip**, 否则测的是「长度算术」而不是协议。 */
    {
        static uint8_t big[HW_DMC_MAX_PAYLOAD + 1];
        const uint8_t* up = NULL;
        uint16_t u_len = 0;
        uint8_t  u_cmd = 0;
        int ok13;
        memset(big, 0x5A, sizeof(big));
        n = hw_dmc_pack(HW_DMC_CMD_DATA, big, HW_DMC_MAX_PAYLOAD, buf, (int)sizeof(buf));
        r = hw_dmc_unpack(buf, n, &u_cmd, &up, &u_len);
        i = hw_dmc_pack(HW_DMC_CMD_DATA, big, HW_DMC_MAX_PAYLOAD + 1, buf, (int)sizeof(buf));
        ok13 = (n == (int)HW_DMC_MAX_FRAME) &&
               (r == HW_DMC_ERR_OK) &&
               (u_cmd == (uint8_t)HW_DMC_CMD_DATA) &&
               (u_len == (uint16_t)HW_DMC_MAX_PAYLOAD) &&
               (up != NULL) &&
               (memcmp(up, big, HW_DMC_MAX_PAYLOAD) == 0) &&
               (i == HW_DMC_ERR_PARAM);
        dmc_t_put(putf, "[13] pack %u=%d roundtrip rc=%d plen=%u;  %u=%d expect=%d (PARAM)\n",
                  (unsigned)HW_DMC_MAX_PAYLOAD, n, r, (unsigned)u_len,
                  (unsigned)(HW_DMC_MAX_PAYLOAD + 1), i, (int)HW_DMC_ERR_PARAM);
        if (!ok13) { fails++; dmc_t_put(putf, "  FAIL payload bound\n", 0, -1); }
    }

    /* ---- [14] init 不清 BSP (坑 #25 回归护栏) ---- */
    {
        static hw_dmc_bsp_t fake;
        static int fake_tx_calls = 0;
        fake.tx = NULL; fake.rx = NULL; fake.now_ms = NULL; fake.rx_ready = NULL;
        hw_dmc_bsp_install(&fake);
        hw_dmc_init(NULL);
        c = (g_bsp.tx == NULL);   /* 装的是空 BSP, init 后应仍「装着的状态」 */
        (void)fake_tx_calls;
        dmc_t_put(putf, "[14] init-keeps-bsp: installed=%d after-init=%d\n", 1, c);
        hw_dmc_bsp_install(NULL);   /* 卸载回默认环回 */
        c = 1;
    }

    /* ---- [15] 握手: 模拟对端应答 (注入, 不走 tx 标记) ----
     * ⚠️ 纪律 (坑 #17 的同类): 用例必须真的走协议路径, 不能只验 pack/unpack。
     *   这里对端 HELLO_ACK 用 loop_inject 预置 (模拟从设备), master 的
     *   HELLO 经 tx 发出, 环回模型**跳过自己发的** → wait 读到注入的 ACK。
     *   判别: 若把 loop_inject 换成 loop_tx (打 tx 标), 本用例必 FAIL。 */
    hw_dmc_loop_reset();
    hw_dmc_link_init(&lk, 0x11, 0x22, 10);
    {
        static uint8_t peer[32];
        int pn = hw_dmc_pack(HW_DMC_CMD_HELLO_ACK, hello, 8, peer, (int)sizeof(peer));
        c = (pn == 14);
        hw_dmc_loop_inject(peer, pn);                  /* 对端 ACK 先进缓冲 */
        r = hw_dmc_handshake_master(&lk, 3);
        dmc_t_put(putf, "[15] handshake rc=%d (%s) state=%s txmark=%d\n",
                  r, hw_dmc_err_name(r), hw_dmc_state_name(hw_dmc_link_state(&lk)),
                  hw_dmc_loop_txmark());
        c = c && (r == HW_DMC_ERR_OK) &&
             (hw_dmc_link_state(&lk) == HW_DMC_ST_ESTABLISHED) &&
             (hw_dmc_loop_txmark() == 14);   /* HELLO 帧 14 字节真发出去了 */
        if (!c) { fails++; dmc_t_put(putf, "  FAIL handshake\n", 0, -1); }
    }

    /* ---- [15b] 文档承诺的命令必须全部可执行 (防"帮助文本撒谎") ----
     * ⚠️ 坑1: frame 曾在 usage 里列着却没实现, 跑起来 unknown cmd,
     *   而 selftest 22 项全绿 —— 自检覆盖不到"文档与实现一致"。
     * ⚠️ 坑2 (本轮实测): [15b] 初版只列 usage 里的 9 条, 于是我自己照
     *   头文件 L229 "双参: frame <cmd> <plen>" 写了 .mo, 全拿 -1 ——
     *   **断言覆盖范围比承诺范围小, 断言就是自欺**。
     *   现在改为遍历**头文件 hw_dmc.h 承诺的完整单参表**, 而非 usage 摘录。
     *   注: help 期望 -2 (非 -1), 故单独判。 */
    {
        static const char* promised[] = {
            "count", "cmds", "states", "errs", "maxpay", "maxframe",
            "minframe", "mode", "frame", "crcvec", "hello", "golden",
            "cmdfnv", "statefnv", "framefnv", "ok", "crclen", "help"
        };
        int bad = 0, k, rc2;
        for (k = 0; k < (int)(sizeof(promised) / sizeof(promised[0])); k++) {
            rc2 = hw_dmc_cmd(promised[k], NULL);
            /* 合法返回 = 非 -1; help 必须是 -2 而非 -1 */
            if (rc2 == -1) {
                bad++;
                dmc_t_put(putf, "  FAIL header promises '%s' but not implemented\n", promised[k], 0, -1);
            } else if (strcmp(promised[k], "help") == 0 && rc2 != -2) {
                bad++;
                dmc_t_put(putf, "  FAIL 'help' must return -2, got %d\n", rc2, 0, -1);
            }
        }
        dmc_t_put(putf, "[15b] header-promise %d cmds, unimplemented=%d\n",
                  (int)(sizeof(promised) / sizeof(promised[0])), bad);
        if (bad) fails++;
    }

    /* ---- [15c] 带数值参数路径 (双参 opcode 的落地) ----
     * 🕳️ 坑 (本轮实测): mo2kbc 双参内置 hw_dmc("fmt", 数值) 编译成
     *   imm=R_TMP+1, VM 侧把数值十进制拼到命令串尾再调进来;
     *   而 cmd 层当时只有 strcmp 精确匹配 → **双参永远返回 -1**,
     *   一条"看起来实现了、实际全线报错"的死路径。
     *   selftest 抓不到它: [15b] 只跑单参命令, 覆盖不到带数值分支。
     * 正向: num/add/sub/mul 必须真参与运算 (不是忽略数值返回常量)。
     * 负向: 未知前缀/无数字/尾随垃圾/双负号/溢出/超长前缀 全部 -1。 */
    {
        struct { const char* c; int want; } num_ok[] = {
            { "num 7",            7 },
            { "num 0",            0 },
            { "num  252",         252 },        /* 多空格 */
            { "add 100",          100 + 9 },     /* +CMD_NUM(9) */
            { "sub 100",          100 - 9 },
            { "mul 2",            2 * (int)HW_DMC_MAX_PAYLOAD }, /* *MAX_PAYLOAD */
            { "num -5",           -5 }           /* 负数 */
        };
        struct { const char* c; } num_bad[] = {
            { "frame 7" },      /* 未知前缀: 不得静默忽略数值后返回 frame */
            { "num" },          /* 无空格 → 已由单参表覆盖, 这里防回归 */
            { "num abc" },      /* 空格后非数字 */
            { "num 7x" },       /* 尾随垃圾 */
            { "num --5" },      /* 双负号 */
            { "num 99999999" }, /* 溢出 */
            { "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz 1" } /* 前缀超 31 字符 */
        };
        int nbad = 0, k, rc3;
        for (k = 0; k < (int)(sizeof(num_ok) / sizeof(num_ok[0])); k++) {
            rc3 = hw_dmc_cmd(num_ok[k].c, NULL);
            if (rc3 != num_ok[k].want) {
                nbad++;
                dmc_t_put(putf, "  FAIL num '%s' want=%d got=%d\n",
                          num_ok[k].c, num_ok[k].want, rc3, 0, -1);
            }
        }
        for (k = 0; k < (int)(sizeof(num_bad) / sizeof(num_bad[0])); k++) {
            rc3 = hw_dmc_cmd(num_bad[k].c, NULL);
            if (rc3 != -1) {
                nbad++;
                dmc_t_put(putf, "  FAIL num-reject '%s' want=-1 got=%d\n",
                          num_bad[k].c, rc3, 0, -1);
            }
        }
        dmc_t_put(putf, "[15c] num-path %d ok / %d reject, bad=%d\n",
                  (int)(sizeof(num_ok) / sizeof(num_ok[0])),
                  (int)(sizeof(num_bad) / sizeof(num_bad[0])), nbad);
        if (nbad) fails++;
    }

    /* ---- [16] 发送前必须已建立 (ERR_STATE) ---- */
    hw_dmc_loop_reset();
    hw_dmc_link_init(&lk, 0x11, 0x22, 10);
    r = hw_dmc_send_data(&lk, (const uint8_t*)"x", 1, 1);
    dmc_t_put(putf, "[16] send-data idle rc=%d (%s) expect=%d (STATE)\n",
              r, hw_dmc_err_name(r), (int)HW_DMC_ERR_STATE);
    if (r != HW_DMC_ERR_STATE) { fails++; dmc_t_put(putf, "  FAIL send idle\n", 0, -1); }

    /* ---- [17] 空链路状态查询必超时 (不挂死, 确定性) ----
     * ⚠️ 这一条同时是「半双工模型」的回归护栏: 若环回把本端自己发的
     *   STATUS 当成应答读回来, 这里会返回 FRAME 而非 TIMEOUT。 */
    hw_dmc_loop_reset();
    hw_dmc_link_init(&lk, 0x11, 0x22, 5);
    r = hw_dmc_query_status(&lk, &status, 5);
    dmc_t_put(putf, "[17] status no-peer rc=%d (%s) expect=TIMEOUT(半双工, 不回环自己)\n",
              r, hw_dmc_err_name(r));
    if (r != HW_DMC_ERR_TIMEOUT) { fails++; dmc_t_put(putf, "  FAIL status timeout\n", 0, -1); }

    /* ---- [18] 统计计数 ([17] 发了一次 STATUS 且超时) ---- */
    {
        uint32_t tx = 0, rx = 0, cerr = 0, terr = 0;
        hw_dmc_stats(&lk, &tx, &rx, &cerr, &terr);
        dmc_t_put(putf, "[18] stats tx=%u rx=%u crc=%u timeout=%u\n",
                  (unsigned)tx, (unsigned)rx, (unsigned)cerr, (unsigned)terr);
        c = (tx == 1 && terr == 1 && rx == 0);
        if (!c) { fails++; dmc_t_put(putf, "  FAIL stats\n", 0, -1); }
    }

    /* ---- [19] cmd 分发 (含双出口一致性: 掩码坑的回归护栏) ---- */
    {
        int a = hw_dmc_cmd("count", NULL), o = hw_dmc_cmd("ok", NULL);
        int g1 = hw_dmc_cmd("golden", NULL);
        dmc_t_put(putf, "[19] cmd count=%d ok=%d mode=%d golden=0x%08X\n",
                  a, o, hw_dmc_cmd("mode", NULL), (unsigned)g1);
        /* ⚠️ 曾踩: cmd 出口对 32 位 FNV 掩了 0x7FFFFFFF 而 selftest 内部没掩,
         *   同一份数据两个出口给出不同值 → 判别性断言: cmd 出口必须等于宏。 */
        c = (a == HW_DMC_CMD_NUM) && (o == 1) &&
            (g1 == (int)HW_DMC_GOLDEN) &&
            (hw_dmc_cmd("cmdfnv", NULL)   == (int)HW_DMC_CMD_FNV) &&
            (hw_dmc_cmd("statefnv", NULL) == (int)HW_DMC_STATE_FNV) &&
            (hw_dmc_cmd("framefnv", NULL) == (int)HW_DMC_FRAME_FNV);
        if (!c) { fails++; dmc_t_put(putf, "  FAIL cmd dispatch\n", 0, -1); }
    }

    /* ---- [20] slave 侧握手 + 数据收发闭环 (端到端走协议) ---- */
    hw_dmc_loop_reset();
    hw_dmc_link_init(&lk, 0x22, 0x11, 10);
    {
        static uint8_t hbuf[HW_DMC_MAX_FRAME], dbuf[64], got[64];
        int hn = hw_dmc_pack(HW_DMC_CMD_HELLO, hello, 8, hbuf, (int)sizeof(hbuf));
        uint16_t rl = 0;
        hw_dmc_loop_inject(hbuf, hn);                   /* 模拟 master 发 HELLO */
        r = hw_dmc_handshake_slave(&lk, 10);
        dmc_t_put(putf, "[20] slave-handshake rc=%d state=%s\n",
                  r, hw_dmc_state_name(hw_dmc_link_state(&lk)));
        c = (r == HW_DMC_ERR_OK && hw_dmc_link_state(&lk) == HW_DMC_ST_ESTABLISHED);
        if (!c) { fails++; dmc_t_put(putf, "  FAIL slave handshake\n", 0, -1); }
        /* 收数据: 注入一帧 DATA(seq=0, "hi"), slave 应剥掉 2 字节 seq 回 "hi" */
        memset(dbuf, 0, sizeof(dbuf));
        dbuf[0] = 0x00; dbuf[1] = 0x00; dbuf[2] = 'h'; dbuf[3] = 'i';
        hn = hw_dmc_pack(HW_DMC_CMD_DATA, dbuf, 4, hbuf, (int)sizeof(hbuf));
        hw_dmc_loop_inject(hbuf, hn);
        memset(got, 0, sizeof(got));
        r = hw_dmc_recv_data(&lk, got, (int)sizeof(got), &rl, 10);
        dmc_t_put(putf, "[20b] slave-recv rc=%d len=%u data=\"%c%c\" seq_rx=%u\n",
                  r, (unsigned)rl, got[0], got[1], (unsigned)lk.seq_rx);
        /* 契约护栏: out 必须是**已剥掉 2 字节 seq** 的业务数据, 且长度一致。
         *   (原实现把含 seq 的整段拷进 out 却报 len-2 = 长度与内容错位) */
        c = (r == HW_DMC_ERR_OK && rl == 2 && got[0] == 'h' && got[1] == 'i' &&
             lk.seq_rx == 1);
        if (!c) { fails++; dmc_t_put(putf, "  FAIL slave recv\n", 0, -1); }
    }

    /* ---- [21] 负向: 上层越界负载必被拒 (PARAM), 不静默截断 ---- */
    {
        static uint8_t over[HW_DMC_MAX_PAYLOAD];   /* MAX_PAYLOAD > (MAX_PAYLOAD-2) = DATA 上限 */
        hw_dmc_link_init(&lk, 0x11, 0x22, 10);
        lk.state = HW_DMC_ST_ESTABLISHED;          /* 绕过 state 检查, 专测上界 */
        memset(over, 0x77, sizeof(over));
        r = hw_dmc_send_data(&lk, over, HW_DMC_MAX_PAYLOAD, 1);
        dmc_t_put(putf, "[21] send MAXPAYB rc=%d (%s) expect=%d (PARAM, 2B 留给 seq)\n",
                  r, hw_dmc_err_name(r), (int)HW_DMC_ERR_PARAM);
        if (r != HW_DMC_ERR_PARAM) { fails++; dmc_t_put(putf, "  FAIL data bound\n", 0, -1); }
    }

    /* ---- [22] 坏 CRC 注入: 对端发来坏帧, 必报 CRC 且计入 err_crc ---- */
    hw_dmc_loop_reset();
    hw_dmc_link_init(&lk, 0x22, 0x11, 10);
    lk.state = HW_DMC_ST_ESTABLISHED;
    {
        static uint8_t bad[32];
        uint16_t rl = 0;
        int bn = hw_dmc_pack(HW_DMC_CMD_DATA, (const uint8_t*)"\x00\x00hi", 4,
                             bad, (int)sizeof(bad));
        bad[bn - 1] ^= 0x01;                        /* 翻 CRC 高字节 */
        hw_dmc_loop_inject(bad, bn);
        r = hw_dmc_recv_data(&lk, dmc_t_scratch, (int)sizeof(dmc_t_scratch), &rl, 10);
        c = (r == HW_DMC_ERR_CRC) && (lk.err_crc == 1);
        dmc_t_put(putf, "[22] bad-frame rc=%d (%s) err_crc=%u\n",
                  r, hw_dmc_err_name(r), (unsigned)lk.err_crc);
        if (!c) { fails++; dmc_t_put(putf, "  FAIL bad frame reject\n", 0, -1); }
    }

    /* ---- [24] 草稿 API 意图层: porto_init / create / ready / run_cmd ----
     * 🕳️ 为什么专门补这组用例: 这 4 个函数是本轮从伪代码底稿新提取实现的,
     *   加完之后跑全跨矩阵, 输出 md5 **纹丝不动**仍是 ef54f2a8…。用 nm 查
     *   确认符号**确实链接进了二进制**, 但那只是"编进去了"; 它们从未被
     *   任何用例调用过 ⇒ 矩阵对它们的行为**零证明力**。
     *   「编译通过」≠「行为正确」, 必须真跑。
     *
     * 覆盖三块:
     *   (a) 参数护栏全负向 —— 静默成功比崩溃更危险
     *   (b) ready 状态判定: 只有 ESTABLISHED 算就绪
     *   (c) NACK 必须被识别为**失败** —— 本轮修掉的那个真 bug 的回归护栏:
     *       run_cmd 走 dmc_link_wait(expect=0) 等"任意命令", 而
     *       dmc_link_wait 只在 `expect != 0` 分支里识别 NACK,
     *       漏判会把「对端拒绝」上报成「成功」。 */
    {
        static uint8_t resp[64];
        static hw_dmc_link_t nk;
        static uint8_t nack[16];
        static uint8_t ackf[16];
        int bad = 0, nn, rlen = -1, nack_rc = -1;

        /* (a) 参数护栏 */
        if (hw_dmc_proto_create(NULL, 1, 2, 10) != HW_DMC_ERR_PARAM) bad++;
        if (hw_dmc_proto_ready(NULL) != HW_DMC_ERR_PARAM) bad++;
        if (hw_dmc_proto_run_cmd(NULL, HW_DMC_CMD_STATUS, NULL, 0, NULL, 0,
                                 &rlen, 10) != HW_DMC_ERR_PARAM) bad++;
        /* payload=NULL 却带长度 = 自相矛盾 */
        if (hw_dmc_proto_run_cmd(&lk, HW_DMC_CMD_STATUS, NULL, 4, NULL, 0,
                                 &rlen, 10) != HW_DMC_ERR_PARAM) bad++;
        /* 负容量 / 超上界容量 (上界 = MAX_PAYLOAD 249, 250 就该拒) */
        if (hw_dmc_proto_run_cmd(&lk, HW_DMC_CMD_STATUS, NULL, 0, resp, -1,
                                 &rlen, 10) != HW_DMC_ERR_PARAM) bad++;
        if (hw_dmc_proto_run_cmd(&lk, HW_DMC_CMD_STATUS, NULL, 0, resp,
                                 (int)HW_DMC_MAX_PAYLOAD + 1, &rlen, 10)
            != HW_DMC_ERR_PARAM) bad++;

        /* (b) ready: IDLE 不算就绪, ESTABLISHED 才算 */
        if (hw_dmc_proto_create(&nk, 0x11, 0x22, 10) != HW_DMC_ERR_OK) bad++;
        if (nk.local_addr != 0x11 || nk.slave_addr != 0x22) bad++;  /* create 须真填址 */
        if (hw_dmc_proto_ready(&nk) != HW_DMC_ERR_STATE) bad++;
        nk.state = HW_DMC_ST_ESTABLISHED;
        if (hw_dmc_proto_ready(&nk) != HW_DMC_ERR_OK) bad++;

        /* (c) NACK 回归护栏: 环回是半双工(读时跳过本端 OWN_TX 槽位),
         *     所以注入的对端 NACK 不会被本端刚发的那帧顶掉。 */
        nn = hw_dmc_pack(HW_DMC_CMD_NACK, (const uint8_t*)"\x00", 1,
                         nack, (int)sizeof(nack));
        if (nn > 0) {
            hw_dmc_loop_inject(nack, nn);
            nk.state = HW_DMC_ST_ESTABLISHED;
            r = hw_dmc_proto_run_cmd(&nk, HW_DMC_CMD_STATUS, NULL, 0,
                                     resp, (int)sizeof(resp), &rlen, 10);
            nack_rc = r;
            /* 期望 ERR_NACK; 拿到 OK 就说明「被拒绝」被误报成「成功」 */
            if (r != HW_DMC_ERR_NACK) bad++;
            if (rlen != 0) bad++;     /* 出错时不得顺手改写长度 */
        } else {
            bad++;
        }

        /* (c2) 长度/错误码撞车护栏 —— 本组用例最初就是为它写的。
         *   🕳️ 初版 run_cmd 成功时 `return (int)rlen`, 于是**1 字节的正常应答
         *      返回 1 = HW_DMC_ERR_TIMEOUT**, 调用方无法区分成功与超时。
         *      变异测试(撤掉 NACK 修复)时暴露: 拿到的 1 被错标成 TIMEOUT,
         *      而它其实是长度。修法 = 长度走 out-param, 返回值恒为 err_t,
         *      与模块既有约定(hw_dmc_recv_data 的 recv_len)一致。 */
        nn = hw_dmc_pack(HW_DMC_CMD_STATUS_RSP, (const uint8_t*)"\xA5", 1,
                         ackf, (int)sizeof(ackf));
        if (nn > 0) {
            hw_dmc_loop_inject(ackf, nn);
            nk.state = HW_DMC_ST_ESTABLISHED;
            rlen = -1;
            r = hw_dmc_proto_run_cmd(&nk, HW_DMC_CMD_STATUS, NULL, 0,
                                     resp, (int)sizeof(resp), &rlen, 10);
            /* 1 字节成功应答: 返回值必须是 OK(0), 长度必须落在 out-param */
            if (r != HW_DMC_ERR_OK) bad++;
            if (rlen != 1) bad++;
            if (resp[0] != 0xA5) bad++;   /* 载荷真传到了, 不只是长度对 */
        } else {
            bad++;
        }
        nk.state = HW_DMC_ST_IDLE;   /* 复原, 不污染后续 */

        dmc_t_put(putf, "[24] draft-API layer bad=%d nack_rc=%d (%s) ok_rc=%d len=%d\n",
                  bad, nack_rc, hw_dmc_err_name(nack_rc), r, rlen);
        if (bad) { fails++; dmc_t_put(putf, "  FAIL draft API layer\n", 0, -1); }
    }

    /* ---- [23] 恢复 BSP (不污染真机扫描, 坑 #25) ---- */
    g_bsp = saved_bsp;

    dmc_t_put(putf, "\nDMC selftest: %s (fails=%d)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails;
}

/* ============================================================
 * CLI  (家族约定: 读 argv[2], 判 argc >= 3)
 * ============================================================ */
static int dmc_cli_puts(const char* s) { return printf("%s", s); }

int hw_dmc_cli(int argc, char** argv)
{
    if (argc < 3) {
        dmc_cli_puts("usage: xiaomo dmc <cmd>\n");
        dmc_cli_puts("  count|cmds|states|errs|golden|frame|ok|crcvec\n");
        dmc_cli_puts("  hello        pack+unpack 自证\n");
        dmc_cli_puts("  selftest     全量自检\n");
        return -1;
    }
    if (strcmp(argv[2], "selftest") == 0) return hw_dmc_selftest(dmc_cli_puts);
    if (strcmp(argv[2], "cmds") == 0) {
        int i;
        for (i = 0; i < HW_DMC_CMD_NUM; i++)
            printf("  %02X %s\n", g_cmd_tab[i], g_cmd_names[i]);
        return 0;
    }
    if (strcmp(argv[2], "states") == 0) {
        int i;
        for (i = 0; i < HW_DMC_STATE_NUM; i++)
            printf("  %d %s\n", i, g_state_names[i]);
        return 0;
    }
    if (strcmp(argv[2], "golden") == 0) {
        /* 🕳️ 坑: ESP32/xtensa/riscv32 上 uint32_t == long unsigned int,
         *   %08X 期望 unsigned int → -Wformat 报错。显式强转 (unsigned)。 */
        printf("golden=0x%08X expect=0x%08X %s\n",
               (unsigned)dmc_golden(), (unsigned)HW_DMC_GOLDEN,
               dmc_golden() == HW_DMC_GOLDEN ? "OK" : "MISMATCH");
        return (dmc_golden() == HW_DMC_GOLDEN) ? 0 : 1;
    }
    if (strcmp(argv[2], "frame") == 0) {
        /* 🕳️ 曾踩: usage 里承诺了 frame 命令却从未实现 → 跑起来报
         *   "unknown cmd", 而帮助文本照旧说它存在 = **文档对用户撒谎**。
         *   自检覆盖不到的静默失败靠"usage 承诺 ⊆ 实际实现"这条纪律堵。 */
        dmc_cli_puts("  frame  = SYNC0|SYNC1|LEN|CMD|PAYLOAD|CRC_LO|CRC_HI\n");
        dmc_cli_puts("  sync = 0xAA 0x55 (2B 定界)  len = 4 + plen + 2\n");
        dmc_cli_puts("  cmd  = 1B 协议命令码 (见 cmds)\n");
        /* 🕳️ 曾写 "CRC-16/MODBUS" = 对用户撒谎: 实际 poly=0x1021 MSB-first
         *   init=0xFFFF 不反射 = CRC-16/CCITT-FALSE (校验值 0x29B1;
         *   MODBUS 是 0x8005 反射式, 校验值 0x4B37)。名称与实现必须对齐。 */
        dmc_cli_puts("  crc  = CRC-16/CCITT-FALSE (poly=0x1021 init=0xFFFF 不反射)\n");
        dmc_cli_puts("         over SYNC0..PAYLOAD (2B 小端)\n");
        printf("  min=%u max=%u max_payload=%u\n",
               (unsigned)HW_DMC_MIN_FRAME, (unsigned)HW_DMC_MAX_FRAME,
               (unsigned)HW_DMC_MAX_PAYLOAD);
        printf("  实测: HELLO(8B payload) pack = %d B\n", 14);
        return 0;
    }
    if (strcmp(argv[2], "crcvec") == 0) {
        printf("crc(\"123456789\")=0x%04X expect=0x%04X\n",
               hw_dmc_crc_of_vector(), (int)HW_DMC_CRC_VECTOR);
        return (hw_dmc_crc_of_vector() == HW_DMC_CRC_VECTOR) ? 0 : 1;
    }
    if (strcmp(argv[2], "hello") == 0) {
        static const uint8_t hello[8] = { 0x01, 0x01, 0xFF, 0xFF, 0, 0, 0, 0 };
        static uint8_t buf[HW_DMC_MAX_FRAME];
        const uint8_t* pl = NULL; uint16_t pl_len = 0; uint8_t cmd = 0;
        int n = hw_dmc_pack(HW_DMC_CMD_HELLO, hello, 8, buf, (int)sizeof(buf));
        int r = hw_dmc_unpack(buf, n, &cmd, &pl, &pl_len);
        printf("pack len=%d unpack rc=%d cmd=%s plen=%d match=%d\n",
               n, r, hw_dmc_cmd_name(cmd), (int)pl_len,
               (r == 0 && pl_len == 8 && memcmp(pl, hello, 8) == 0));
        return r;
    }
    {
        int rc = hw_dmc_cmd(argv[2], NULL);
        if (rc == -2) { dmc_cli_puts("count|cmds|states|errs|golden|frame|ok|crcvec|hello|selftest\n"); return 0; }
        if (rc < 0) { printf("dmc: unknown cmd '%s'\n", argv[2]); return -1; }
        printf("%d\n", rc);
        return 0;
    }
}
