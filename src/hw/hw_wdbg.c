/*
 * xiaomo - 无线调试器信号层 (hw_wdbg, 2026-09-29)
 *
 * 复刻对象: 立创开源硬件「【AI设计】AI远程调试器」(ESP32-S3 无线串口调试器)
 *   原始能力: 串口桥接(TCP:3333) + PWM/SPI/I2C 多协议信号监控 + LCD 菜单
 *             + Web/HTTP/WS + MCP 接入  (AI 全流程开发的硬件项目)
 *
 * 本模块承载其「信号面」核心 (与硬件/网络无关的算法与状态机部分):
 *   1. 串口桥接   : 2048B 环形流缓冲 + 波特率白名单 + RX/TX 计数 + 溢出丢弃
 *   2. PWM 监控   : 输出 (LEDC 语义) + 测频/占空比 (双沿语义)
 *   3. SPI 监控   : Master 传输 + Slave 捕获环形缓冲 (50 条 × 64B) + Mode 0~3
 *   4. I2C 监控   : Master 读/写 + Slave/Passive 捕获 + 7bit 地址 + 方向位
 *   5. 自定义线序 : UART/SPI/I2C/PWM 四协议引脚线序可改/可复位
 *
 * 全跨式设计 (与 hw_token / hw_fault / hw_core 同款):
 *   1. 编译期模式探测六模式, 构建系统可用 -DHW_WDBG_MODE_OVERRIDE=n 强指
 *      (KELL 内核内 __linux__ 亦须 -DHW_WDBG_KELL=1 抢先识别)。
 *   2. 核心仅依赖 stdint+string+stdio, 真机可 freestanding 编译。
 *   3. 底层收发走 BSP 回调: 默认 = 确定性模拟器 (全平台逐位一致可回归);
 *      真机固件 hw_wdbg_bsp_install() 注入真实 UART/SPI/I2C/PWM, 上层零改动。
 *   4. 黄金参考: 默认线序表 FNV-1a-32 = HW_WDBG_GOLDEN (Python 对拍锁定),
 *      selftest 逐项断言, 防未来误改破坏跨历史一致。
 *
 * 六层接入 (2026-09-29):  Makefile / OP_HW_WDBG_CALL /
 *   mo2kbc 内置 hw_wdbg() / CLI ./xiaomo wdbg / examples/wdbg_test.mo /
 *   tests/run_tests.sh wdbg 块
 */
#include "hw_wdbg.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t wd_mode_probe(void)
{
#if defined(HW_WDBG_MODE_OVERRIDE)
    return (uint8_t)HW_WDBG_MODE_OVERRIDE;
#elif defined(HW_WDBG_KELL)
    return (uint8_t)HW_WDBG_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_WDBG_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)   || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_WDBG_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_WDBG_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_WDBG_MODE_HOST;
#else
    return (uint8_t)HW_WDBG_MODE_TEST;
#endif
}

uint8_t hw_wdbg_mode(void)
{
    static uint8_t cached = 0xFFu;
    if (cached == 0xFFu) cached = wd_mode_probe();
    return cached;
}

static const char* const wd_mode_names[HW_WDBG_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_wdbg_mode_str(uint8_t mode)
{
    return (mode < HW_WDBG_MODE_MAX) ? wd_mode_names[mode] : "?";
}

static const char* const wd_proto_names[HW_WDBG_PROTO_MAX] = {
    "UART", "SPI", "I2C", "PWM"
};

const char* hw_wdbg_proto_name(uint8_t proto)
{
    return (proto < HW_WDBG_PROTO_MAX) ? wd_proto_names[proto] : "?";
}

const char* hw_wdbg_result_code_str(int rc)
{
    switch (rc) {
    case HW_WDBG_R_OK:     return "OK";
    case HW_WDBG_R_NOARGS: return "NOARGS";
    case HW_WDBG_R_BADARG: return "BADARG";
    case HW_WDBG_R_IOERR:  return "IOERR";
    case HW_WDBG_R_STATE:  return "STATE";
    case HW_WDBG_R_NOCMD:  return "NOCMD";
    case HW_WDBG_R_HELP:   return "HELP";
    default:               return "?";
    }
}

/* ============================================================
 * 2. 默认线序表 (引脚分配取自原件 J3 接口 + V1.1 引脚)
 *    黄金校验和由独立 Python 实现锁定 = HW_WDBG_GOLDEN
 * ============================================================ */
static const hw_wdbg_pin_t g_pin_default[HW_WDBG_PROTO_MAX] = {
    /* proto, name, lines, npins, pins[8], rate */
    { HW_WDBG_PROTO_UART, "UART", "TX,RX",           2, { 47, 21, 0, 0, 0, 0, 0, 0 }, 115200u  },
    { HW_WDBG_PROTO_SPI,  "SPI",  "SCK,MOSI,MISO,CS", 4, { 12, 11, 13, 10, 0, 0, 0, 0 }, 1000000u },
    { HW_WDBG_PROTO_I2C,  "I2C",  "SCL,SDA",          2, {  9,  8, 0, 0, 0, 0, 0, 0 }, 400000u  },
    { HW_WDBG_PROTO_PWM,  "PWM",  "SIG",              1, { 18,  0, 0, 0, 0, 0, 0, 0 }, 1000u    }
};

/* 活动线序 (pin set 可改; init 复位为默认副本) */
static hw_wdbg_pin_t g_pin_active[HW_WDBG_PROTO_MAX];

const hw_wdbg_pin_t* hw_wdbg_pin_table(void) { return g_pin_default; }
const hw_wdbg_pin_t* hw_wdbg_pin_active(uint8_t proto)
{
    return (proto < HW_WDBG_PROTO_MAX) ? &g_pin_active[proto] : NULL;
}

/* FNV-1a-32 over (name + lines + npins + pins 逐模) + proto — 与 Python 对拍 */
uint32_t hw_wdbg_pin_checksum(void)
{
    uint32_t h = 0x811C9DC5u;
    uint32_t i;
    for (i = 0; i < HW_WDBG_PROTO_MAX; i++) {
        const hw_wdbg_pin_t* p = &g_pin_default[i];
        const unsigned char* s = (const unsigned char*)p->name;
        uint32_t k;
        while (*s) { h ^= (uint32_t)*s++; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
        s = (const unsigned char*)p->lines;
        while (*s) { h ^= (uint32_t)*s++; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
        h ^= (uint32_t)p->npins; h = (h * 0x01000193u) & 0xFFFFFFFFu;
        for (k = 0; k < (uint32_t)p->npins; k++) {
            h ^= (uint32_t)p->pins[k];
            h = (h * 0x01000193u) & 0xFFFFFFFFu;
        }
        h ^= (uint32_t)p->proto; h = (h * 0x01000193u) & 0xFFFFFFFFu;
    }
    return h;
}

/* ============================================================
 * 3. 波特率档位白名单 (原件 9600/115200/460800/921600)
 * ============================================================ */
static const uint32_t g_bauds[HW_WDBG_BAUD_MAX] = {
    9600u, 115200u, 460800u, 921600u
};

uint32_t hw_wdbg_baud_at(uint32_t idx)
{
    return (idx < HW_WDBG_BAUD_MAX) ? g_bauds[idx] : 0u;
}

int hw_wdbg_baud_ok(uint32_t baud)
{
    uint32_t i;
    for (i = 0; i < HW_WDBG_BAUD_MAX; i++)
        if (g_bauds[i] == baud) return 1;
    return 0;
}

/* ============================================================
 * 4. 串口流缓冲 (2048B 环形) + 事务环形缓冲 (50 × 64B)
 * ============================================================ */
static uint8_t  g_stream[HW_WDBG_STREAM_CAP];
static uint32_t g_stream_len;     /* 待读字节 (线性使用, 读走即前移) */
static uint32_t g_stream_tail;    /* 数据尾部游标 */
static uint8_t  g_scratch[HW_WDBG_STREAM_CAP + 8];

static hw_wdbg_trace_t g_trace[HW_WDBG_TRACE_MAX];
static uint32_t g_trace_head;     /* 下一个写入槽 (环形) */
static uint32_t g_trace_stored;   /* 当前在环内的条数 (<= TRACE_MAX) */
static uint32_t g_trace_total;    /* 累计推入条数 (含被覆盖) */

static hw_wdbg_stat_t g_st;
static uint8_t  g_spi_mode;       /* SPI 当前 Mode (0..3) */

/* ---- BSP ---- */
static const hw_wdbg_bsp_t* g_bsp;      /* NULL = 用默认模拟器 */

/* ---- 注入用确定值 (模拟器侧可编排, 便于 selftest/回归) ---- */
static uint32_t g_inj_pwm_hz;
static uint32_t g_inj_pwm_duty;

/* ---- 确定性时间戳 (不依赖真实时钟 → 跨模式逐位一致) ---- */
static uint32_t g_ts_us;

/* ---- 结果文本 ---- */
static char g_res[256];

static void wd_res(const char* fmt, ...);

/* ============================================================
 * 5. 默认模拟器 BSP (确定性, 全平台一致)
 * ============================================================ */

/* 确定性 DUT 应答脚本: 打开串口后预置 4 字节 "OK\r\n" (原件风格的握手) */
static const uint8_t wd_dut_greeting[4] = { 'O', 'K', 0x0D, 0x0A };

static int wd_sim_uart_open(uint32_t baud)
{
    (void)baud;
    /* 开桥 → 模拟 DUT 主动发一段问候 (确定性, 供 bridge rd 读到) */
    g_stream_len = 0;
    g_stream_tail = 0;
    memcpy(g_stream, wd_dut_greeting, sizeof(wd_dut_greeting));
    g_stream_len = (uint32_t)sizeof(wd_dut_greeting);
    g_stream_tail = g_stream_len;
    return 0;
}
static int wd_sim_uart_close(void) { return 0; }

static int wd_sim_uart_write(const uint8_t* d, uint32_t n)
{
    /* 模拟 DUT 收到即回显 (loopback), 追加到流缓冲尾 */
    uint32_t i;
    if (d == NULL) return -1;
    for (i = 0; i < n; i++) {
        if (g_stream_tail >= HW_WDBG_STREAM_CAP) { g_st.spill++; continue; }
        g_stream[g_stream_tail++] = d[i];
        g_stream_len++;
    }
    return (int)n;
}

static int wd_sim_uart_read(uint8_t* d, uint32_t cap)
{
    uint32_t take = (g_stream_len < cap) ? g_stream_len : cap;
    if (d == NULL || take == 0) return 0;
    /* 从头部取走 take 字节, 剩余前移 */
    memcpy(d, g_stream, take);
    if (take < g_stream_tail)
        memmove(g_stream, g_stream + take, (size_t)(g_stream_tail - take));
    g_stream_tail -= take;
    g_stream_len  -= take;
    return (int)take;
}

static int wd_sim_pwm_out(uint32_t hz, uint32_t duty)
{
    /* 输出即视为可被自身测量到 (LEDC 自环) */
    g_inj_pwm_hz = hz;
    g_inj_pwm_duty = duty;
    return 0;
}

static int wd_sim_pwm_meas(uint32_t* hz, uint32_t* duty)
{
    if (hz)   *hz   = g_inj_pwm_hz;
    if (duty) *duty = g_inj_pwm_duty;
    return 0;
}

/* SPI 传输: 模拟从设备把每字节异或 0xA5 后回读 (确定性, 便于断言) */
static int wd_sim_spi_xfer(uint8_t mode, const uint8_t* tx, uint8_t* rx, uint32_t n)
{
    uint32_t i;
    (void)mode;
    for (i = 0; i < n; i++)
        rx[i] = (uint8_t)((tx ? tx[i] : 0u) ^ 0xA5u);
    return (int)n;
}

/* I2C 传输: 写=从设备 ACK 计数; 读=从地址起递增的确定字节 */
static int wd_sim_i2c_xfer(uint8_t addr7, uint8_t rd, const uint8_t* tx, uint8_t* rx, uint32_t n)
{
    uint32_t i;
    (void)tx;
    if (rd) {
        for (i = 0; i < n; i++) rx[i] = (uint8_t)((addr7 + i) & 0xFFu);
    }
    return (int)n;
}

static const hw_wdbg_bsp_t g_sim_bsp = {
    wd_sim_uart_open, wd_sim_uart_close, wd_sim_uart_write, wd_sim_uart_read,
    wd_sim_pwm_out, wd_sim_pwm_meas, wd_sim_spi_xfer, wd_sim_i2c_xfer
};

/* 取当前 BSP (注入优先, 否则模拟器) */
static const hw_wdbg_bsp_t* wd_bsp(void)
{
    return (g_bsp != NULL) ? g_bsp : &g_sim_bsp;
}

/* ============================================================
 * 6. 小工具: 结果文本 / 分词 / hex 编解码
 * ============================================================ */
#include <stdarg.h>

static void wd_res(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_res, sizeof(g_res), fmt, ap);
    va_end(ap);
}

void hw_wdbg_result(char* buf, uint32_t cap)
{
    if (buf == NULL || cap == 0u) return;
    snprintf(buf, cap, "%s", g_res);
}

/* 分词: 空白分隔, 最多 8 词; 词表为模块级静态 (无分配) */
static char  g_cmdline[256];
static char* g_tok[8];

static int wd_tok(const char* cmd)
{
    int nt = 0;
    char* p;
    size_t n = strlen(cmd);
    if (n >= sizeof(g_cmdline)) n = sizeof(g_cmdline) - 1u;
    memcpy(g_cmdline, cmd, n);
    g_cmdline[n] = '\0';
    memset(g_tok, 0, sizeof(g_tok));
    p = g_cmdline;
    while (*p != '\0' && nt < 8) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '\0') break;
        g_tok[nt++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p != '\0') *p++ = '\0';
    }
    return nt;
}

/* hex 串 → 字节 ("3c 00 01" / "3c0001" 均可); 返回字节数, -1 = 非法 */
static int wd_hex_parse(const char* s, uint8_t* out, int maxn)
{
    int n = 0;
    if (s == NULL) return -1;
    while (*s != '\0' && n < maxn) {
        unsigned v;
        while (*s == ' ' || *s == ',' || *s == '-' || *s == ':') s++;
        if (*s == '\0') break;
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

/* 字节 → hex 串 (最多 maxb 字节, 超出以 "...(n)" 结尾) */
static void wd_hex_fmt(const uint8_t* d, uint32_t n, uint32_t maxb, char* out, uint32_t cap)
{
    uint32_t i, off = 0;
    if (cap == 0u) return;
    out[0] = '\0';
    for (i = 0; i < n && i < maxb; i++) {
        int w = snprintf(out + off, cap - off, "%s%02x", (i ? " " : ""), (unsigned)d[i]);
        if (w < 0 || (uint32_t)w >= cap - off) { off = cap - 1u; break; }
        off += (uint32_t)w;
    }
    if (i < n) snprintf(out + off, cap - off, " ...(%u)", (unsigned)n);
}

/* 大小写不敏感的 ASCII 字符串相等 (freestanding 友好, 不用 strcasecmp) */
static int wd_ci_eq(const char* a, const char* b)
{
    while (*a != '\0' && *b != '\0') {
        int ca = (*a >= 'a' && *a <= 'z') ? (*a - 32) : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? (*b - 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* 协议名 → 枚举 (大小写不敏感: uart/UART/Uart 均可; 未知返回 PROTO_MAX) */
static uint8_t wd_proto_of(const char* s)
{
    uint8_t i;
    for (i = 0; i < HW_WDBG_PROTO_MAX; i++)
        if (wd_ci_eq(s, wd_proto_names[i])) return i;
    return (uint8_t)HW_WDBG_PROTO_MAX;
}

/* 把 g_tok[from..nt-1] 以空格拼接到 out (多字节 hex 数据用: "01 02 03") */
static void wd_join(int nt, int from, char* out, uint32_t cap)
{
    int i;
    uint32_t off = 0;
    if (cap == 0u) return;
    out[0] = '\0';
    for (i = from; i < nt; i++) {
        int w = snprintf(out + off, cap - off, "%s%s", (i > from ? " " : ""), g_tok[i]);
        if (w < 0 || (uint32_t)w >= cap - off) break;
        off += (uint32_t)w;
    }
}

/* ============================================================
 * 7. 事务记录 (环形) / 状态
 * ============================================================ */
int hw_wdbg_trace_push(const hw_wdbg_trace_t* t)
{
    if (t == NULL) return -1;
    g_trace[g_trace_head] = *t;
    g_trace_head = (g_trace_head + 1u) % HW_WDBG_TRACE_MAX;
    if (g_trace_stored < HW_WDBG_TRACE_MAX) g_trace_stored++;
    g_trace_total++;
    if (t->proto == HW_WDBG_PROTO_SPI)      g_st.spi_traces++;
    else if (t->proto == HW_WDBG_PROTO_I2C) g_st.i2c_traces++;
    return 0;
}

static void wd_trace_clear(void) { g_st.spi_traces = 0; g_st.i2c_traces = 0; }

void hw_wdbg_stat(hw_wdbg_stat_t* out)
{
    if (out == NULL) return;
    g_st.stream_len = g_stream_len;
    *out = g_st;
}

/* ============================================================
 * 8. 生命周期
 * ============================================================ */
void hw_wdbg_hook(void* arg) { (void)arg; }

void hw_wdbg_bsp_install(const hw_wdbg_bsp_t* bsp) { g_bsp = bsp; }

void hw_wdbg_init(void* arg)
{
    uint8_t i;
    (void)arg;
    memset(&g_st, 0, sizeof(g_st));
    memset(g_stream, 0, sizeof(g_stream));
    memset(g_trace, 0, sizeof(g_trace));
    memset(g_scratch, 0, sizeof(g_scratch));
    memset(g_res, 0, sizeof(g_res));
    g_stream_len = 0; g_stream_tail = 0;
    g_trace_head = 0; g_trace_stored = 0; g_trace_total = 0;
    g_inj_pwm_hz = 0; g_inj_pwm_duty = 0;
    g_ts_us = 0;
    g_spi_mode = 0u;
    g_bsp = NULL;                       /* 还原默认模拟器 (旧注入不跨运行残留) */
    for (i = 0; i < HW_WDBG_PROTO_MAX; i++) g_pin_active[i] = g_pin_default[i];
}

/* ============================================================
 * 9. 子命令实现
 * ============================================================ */
static int wd_cmd_bridge(int nt)
{
    const char* sub = (nt >= 2) ? g_tok[1] : "";

    if (strcmp(sub, "open") == 0) {
        long b;
        if (nt < 3) { wd_res("bridge open: 缺波特率"); return HW_WDBG_R_NOARGS; }
        b = strtol(g_tok[2], NULL, 10);
        if (b <= 0 || !hw_wdbg_baud_ok((uint32_t)b)) {
            wd_res("bridge open: 波特率须为 9600/115200/460800/921600");
            return HW_WDBG_R_BADARG;
        }
        if (wd_bsp()->uart_open((uint32_t)b) != 0) { wd_res("uart_open 失败"); return HW_WDBG_R_IOERR; }
        g_st.baud = (uint32_t)b;
        wd_res("bridge OPEN @%ld", b);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "close") == 0) {
        if (g_st.baud == 0u) { wd_res("bridge close: 未打开"); return HW_WDBG_R_STATE; }
        (void)wd_bsp()->uart_close();
        g_st.baud = 0u;
        g_stream_len = 0; g_stream_tail = 0;
        wd_res("bridge CLOSE (tx=%u rx=%u spill=%u)",
               (unsigned)g_st.tx_bytes, (unsigned)g_st.rx_bytes, (unsigned)g_st.spill);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "baud") == 0) {
        long b;
        if (nt < 3) { wd_res("bridge baud: 缺波特率"); return HW_WDBG_R_NOARGS; }
        if (g_st.baud == 0u) { wd_res("bridge baud: 未打开"); return HW_WDBG_R_STATE; }
        b = strtol(g_tok[2], NULL, 10);
        if (b <= 0 || !hw_wdbg_baud_ok((uint32_t)b)) { wd_res("bridge baud: 档位非法"); return HW_WDBG_R_BADARG; }
        (void)wd_bsp()->uart_close();
        if (wd_bsp()->uart_open((uint32_t)b) != 0) { wd_res("uart 重开失败"); return HW_WDBG_R_IOERR; }
        g_st.baud = (uint32_t)b;
        wd_res("bridge BAUD -> %ld", b);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "wr") == 0) {
        int n;
        char hx[256];
        if (g_st.baud == 0u) { wd_res("bridge wr: 未打开"); return HW_WDBG_R_STATE; }
        if (nt < 3) { wd_res("bridge wr: 缺数据"); return HW_WDBG_R_NOARGS; }
        wd_join(nt, 2, hx, sizeof(hx));
        n = wd_hex_parse(hx, g_scratch, (int)HW_WDBG_STREAM_CAP);
        if (n <= 0) { wd_res("bridge wr: hex 非法"); return HW_WDBG_R_BADARG; }
        {
            int w = wd_bsp()->uart_write(g_scratch, (uint32_t)n);
            if (w < 0) { wd_res("uart_write 失败"); return HW_WDBG_R_IOERR; }
            g_st.tx_bytes += (uint32_t)w;
            wd_res("bridge WR %d 字节 (tx=%u)", w, (unsigned)g_st.tx_bytes);
        }
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "rd") == 0) {
        uint32_t want = HW_WDBG_STREAM_CAP;
        int got;
        char hex[200];
        if (g_st.baud == 0u) { wd_res("bridge rd: 未打开"); return HW_WDBG_R_STATE; }
        if (nt >= 3) {
            long l = strtol(g_tok[2], NULL, 10);
            if (l > 0) want = ((uint32_t)l > HW_WDBG_STREAM_CAP) ? HW_WDBG_STREAM_CAP : (uint32_t)l;
        }
        got = wd_bsp()->uart_read(g_scratch, want);
        if (got < 0) { wd_res("uart_read 失败"); return HW_WDBG_R_IOERR; }
        g_st.rx_bytes += (uint32_t)got;
        wd_hex_fmt(g_scratch, (uint32_t)got, 24u, hex, sizeof(hex));
        wd_res("bridge RD %d 字节: %s (rx=%u)", got, hex, (unsigned)g_st.rx_bytes);
        return (got == 0) ? HW_WDBG_R_STATE : HW_WDBG_R_OK;
    }
    if (strcmp(sub, "stat") == 0) {
        wd_res("bridge stat: baud=%u tx=%u rx=%u stream=%u spill=%u",
               (unsigned)g_st.baud, (unsigned)g_st.tx_bytes, (unsigned)g_st.rx_bytes,
               (unsigned)g_stream_len, (unsigned)g_st.spill);
        return HW_WDBG_R_OK;
    }
    wd_res("bridge: 未知子命令 %s", sub);
    return HW_WDBG_R_BADARG;
}

static int wd_cmd_pwm(int nt)
{
    const char* sub = (nt >= 2) ? g_tok[1] : "";

    if (strcmp(sub, "out") == 0) {
        long hz, duty;
        if (nt < 4) { wd_res("pwm out: 用法 out <hz> <duty_permille>"); return HW_WDBG_R_NOARGS; }
        hz = strtol(g_tok[2], NULL, 10);
        duty = strtol(g_tok[3], NULL, 10);
        if (hz <= 0 || hz > 50000 || duty < 0 || duty > 1000) {
            wd_res("pwm out: hz 1..50000, duty 0..1000 (千分比)"); return HW_WDBG_R_BADARG;
        }
        if (wd_bsp()->pwm_out((uint32_t)hz, (uint32_t)duty) != 0) { wd_res("pwm_out 失败"); return HW_WDBG_R_IOERR; }
        wd_res("pwm OUT %ldHz duty=%ld/1000", hz, duty);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "off") == 0) {
        (void)wd_bsp()->pwm_out(0u, 0u);
        g_inj_pwm_hz = 0; g_inj_pwm_duty = 0;
        wd_res("pwm OFF");
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "inject") == 0) {
        long hz, duty;
        if (nt < 4) { wd_res("pwm inject: 用法 inject <hz> <duty_permille>"); return HW_WDBG_R_NOARGS; }
        hz = strtol(g_tok[2], NULL, 10);
        duty = strtol(g_tok[3], NULL, 10);
        if (hz < 0 || hz > 50000 || duty < 0 || duty > 1000) { wd_res("pwm inject: 参数越界"); return HW_WDBG_R_BADARG; }
        g_inj_pwm_hz = (uint32_t)hz; g_inj_pwm_duty = (uint32_t)duty;
        wd_res("pwm INJECT %ldHz duty=%ld/1000", hz, duty);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "meas") == 0) {
        uint32_t hz = 0, duty = 0;
        if (wd_bsp()->pwm_meas(&hz, &duty) != 0) { wd_res("pwm_meas 失败"); return HW_WDBG_R_IOERR; }
        g_st.pwm_hz = hz; g_st.pwm_duty = duty;
        wd_res("pwm MEAS %uHz duty=%u/1000", (unsigned)hz, (unsigned)duty);
        return (hz == 0u) ? HW_WDBG_R_STATE : HW_WDBG_R_OK;
    }
    if (strcmp(sub, "stat") == 0) {
        wd_res("pwm stat: hz=%u duty=%u/1000", (unsigned)g_st.pwm_hz, (unsigned)g_st.pwm_duty);
        return HW_WDBG_R_OK;
    }
    wd_res("pwm: 未知子命令 %s", sub);
    return HW_WDBG_R_BADARG;
}

/* g_spi_mode 定义见第 4 节 (供 init 复位) */

static int wd_cmd_spi(int nt)
{
    const char* sub = (nt >= 2) ? g_tok[1] : "";

    if (strcmp(sub, "mode") == 0) {
        long m;
        if (nt < 3) { wd_res("spi mode: 缺模式 (0..3)"); return HW_WDBG_R_NOARGS; }
        m = strtol(g_tok[2], NULL, 10);
        if (m < 0 || m > 3) { wd_res("spi mode: 须 0..3"); return HW_WDBG_R_BADARG; }
        g_spi_mode = (uint8_t)m;
        wd_res("spi MODE %ld", m);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "xfer") == 0) {
        int n;
        uint8_t rx[HW_WDBG_TRACE_BYTES];
        hw_wdbg_trace_t t;
        char hex[200];
        char hx[256];
        if (nt < 3) { wd_res("spi xfer: 缺 tx 数据"); return HW_WDBG_R_NOARGS; }
        memset(g_scratch, 0, sizeof(g_scratch));
        wd_join(nt, 2, hx, sizeof(hx));
        n = wd_hex_parse(hx, g_scratch, (int)HW_WDBG_TRACE_BYTES);
        if (n <= 0) { wd_res("spi xfer: hex 非法或超 %u 字节", (unsigned)HW_WDBG_TRACE_BYTES); return HW_WDBG_R_BADARG; }
        if (wd_bsp()->spi_xfer(g_spi_mode, g_scratch, rx, (uint32_t)n) < 0) { wd_res("spi_xfer 失败"); return HW_WDBG_R_IOERR; }
        /* 记录到捕获环 (Master 侧事务, dir=0 写) */
        memset(&t, 0, sizeof(t));
        t.ts_us = (g_ts_us += 1000u);
        t.proto = HW_WDBG_PROTO_SPI;
        t.dir   = 0;
        t.addr  = 0;
        t.mode  = g_spi_mode;
        t.len   = (uint16_t)n;
        memcpy(t.data, g_scratch, (size_t)n);
        (void)hw_wdbg_trace_push(&t);
        wd_hex_fmt(rx, (uint32_t)n, 24u, hex, sizeof(hex));
        wd_res("spi XFER mode=%u rx: %s", (unsigned)g_spi_mode, hex);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "list") == 0) {
        wd_res("spi list: stored=%u/%u total=%u (mode=%u)",
               (unsigned)g_trace_stored, (unsigned)HW_WDBG_TRACE_MAX,
               (unsigned)g_trace_total, (unsigned)g_spi_mode);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "clr") == 0) {
        g_trace_head = 0; g_trace_stored = 0; g_trace_total = 0;
        wd_trace_clear();
        wd_res("spi CLR");
        return HW_WDBG_R_OK;
    }
    wd_res("spi: 未知子命令 %s", sub);
    return HW_WDBG_R_BADARG;
}

static int wd_cmd_i2c(int nt)
{
    const char* sub = (nt >= 2) ? g_tok[1] : "";

    if (strcmp(sub, "wr") == 0) {
        long a;
        int n;
        hw_wdbg_trace_t t;
        char hx[256];
        if (nt < 4) { wd_res("i2c wr: 用法 wr <addr7> <hex...>"); return HW_WDBG_R_NOARGS; }
        a = strtol(g_tok[2], NULL, 0);
        if (a < 0 || a > 127) { wd_res("i2c wr: addr7 须 0..127"); return HW_WDBG_R_BADARG; }
        memset(g_scratch, 0, sizeof(g_scratch));
        wd_join(nt, 3, hx, sizeof(hx));
        n = wd_hex_parse(hx, g_scratch, (int)HW_WDBG_TRACE_BYTES);
        if (n <= 0) { wd_res("i2c wr: hex 非法或超 %u 字节", (unsigned)HW_WDBG_TRACE_BYTES); return HW_WDBG_R_BADARG; }
        if (wd_bsp()->i2c_xfer((uint8_t)a, 0u, g_scratch, NULL, (uint32_t)n) < 0) { wd_res("i2c_xfer 失败"); return HW_WDBG_R_IOERR; }
        memset(&t, 0, sizeof(t));
        t.ts_us = (g_ts_us += 1000u);
        t.proto = HW_WDBG_PROTO_I2C;
        t.dir   = 0;
        t.addr  = (uint8_t)a;
        t.len   = (uint16_t)n;
        memcpy(t.data, g_scratch, (size_t)n);
        (void)hw_wdbg_trace_push(&t);
        wd_res("i2c WR addr=0x%02lX %d 字节", a, n);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "rd") == 0) {
        long a, l;
        uint8_t rx[HW_WDBG_TRACE_BYTES];
        hw_wdbg_trace_t t;
        char hex[200];
        if (nt < 4) { wd_res("i2c rd: 用法 rd <addr7> <len>"); return HW_WDBG_R_NOARGS; }
        a = strtol(g_tok[2], NULL, 0);
        l = strtol(g_tok[3], NULL, 10);
        if (a < 0 || a > 127) { wd_res("i2c rd: addr7 须 0..127"); return HW_WDBG_R_BADARG; }
        if (l <= 0 || l > (long)HW_WDBG_TRACE_BYTES) { wd_res("i2c rd: len 1..%u", (unsigned)HW_WDBG_TRACE_BYTES); return HW_WDBG_R_BADARG; }
        if (wd_bsp()->i2c_xfer((uint8_t)a, 1u, NULL, rx, (uint32_t)l) < 0) { wd_res("i2c_xfer 失败"); return HW_WDBG_R_IOERR; }
        memset(&t, 0, sizeof(t));
        t.ts_us = (g_ts_us += 1000u);
        t.proto = HW_WDBG_PROTO_I2C;
        t.dir   = 1;
        t.addr  = (uint8_t)a;
        t.len   = (uint16_t)l;
        memcpy(t.data, rx, (size_t)l);
        (void)hw_wdbg_trace_push(&t);
        wd_hex_fmt(rx, (uint32_t)l, 24u, hex, sizeof(hex));
        wd_res("i2c RD addr=0x%02lX %ld 字节: %s", a, l, hex);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "list") == 0) {
        wd_res("i2c list: stored=%u/%u total=%u (spi=%u)",
               (unsigned)g_trace_stored, (unsigned)HW_WDBG_TRACE_MAX,
               (unsigned)g_trace_total, (unsigned)g_st.spi_traces);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "clr") == 0) {
        g_trace_head = 0; g_trace_stored = 0; g_trace_total = 0;
        wd_trace_clear();
        wd_res("i2c CLR");
        return HW_WDBG_R_OK;
    }
    wd_res("i2c: 未知子命令 %s", sub);
    return HW_WDBG_R_BADARG;
}

static int wd_cmd_pin(int nt)
{
    const char* sub = (nt >= 2) ? g_tok[1] : "";

    if (strcmp(sub, "def") == 0) {
        wd_res("%s:%u,%u %s:%u,%u,%u,%u %s:%u,%u %s:%u",
               g_pin_default[0].name, (unsigned)g_pin_default[0].pins[0], (unsigned)g_pin_default[0].pins[1],
               g_pin_default[1].name, (unsigned)g_pin_default[1].pins[0], (unsigned)g_pin_default[1].pins[1],
               (unsigned)g_pin_default[1].pins[2], (unsigned)g_pin_default[1].pins[3],
               g_pin_default[2].name, (unsigned)g_pin_default[2].pins[0], (unsigned)g_pin_default[2].pins[1],
               g_pin_default[3].name, (unsigned)g_pin_default[3].pins[0]);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "get") == 0) {
        uint8_t p;
        const hw_wdbg_pin_t* a;
        char buf[128];
        uint32_t off = 0, k;
        if (nt < 3) { wd_res("pin get: 缺协议 (uart/spi/i2c/pwm)"); return HW_WDBG_R_NOARGS; }
        p = wd_proto_of(g_tok[2]);
        if (p >= HW_WDBG_PROTO_MAX) { wd_res("pin get: 未知协议 %s", g_tok[2]); return HW_WDBG_R_BADARG; }
        a = &g_pin_active[p];
        buf[0] = '\0';
        for (k = 0; k < a->npins; k++) {
            int w = snprintf(buf + off, sizeof(buf) - off, "%s%u", (k ? "," : ""),
                             (unsigned)a->pins[k]);
            if (w < 0 || (uint32_t)w >= sizeof(buf) - off) break;
            off += (uint32_t)w;
        }
        wd_res("pin %s %s -> %s", a->name, a->lines, buf);
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "set") == 0) {
        uint8_t p;
        hw_wdbg_pin_t* a;
        int i, cnt;
        if (nt < 4) { wd_res("pin set: 用法 set <协议> <引脚...>"); return HW_WDBG_R_NOARGS; }
        p = wd_proto_of(g_tok[2]);
        if (p >= HW_WDBG_PROTO_MAX) { wd_res("pin set: 未知协议 %s", g_tok[2]); return HW_WDBG_R_BADARG; }
        cnt = nt - 3;
        if (cnt != (int)g_pin_default[p].npins) {
            wd_res("pin set: %s 需 %u 个引脚, 收到 %d", g_pin_default[p].name,
                   (unsigned)g_pin_default[p].npins, cnt);
            return HW_WDBG_R_BADARG;
        }
        a = &g_pin_active[p];
        for (i = 0; i < cnt; i++) {
            long v = strtol(g_tok[3 + i], NULL, 10);
            if (v < 0 || v > 48) { wd_res("pin set: 引脚须 0..48"); return HW_WDBG_R_BADARG; }
            a->pins[i] = (uint8_t)v;
        }
        {
            char buf[128];
            uint32_t off = 0, k;
            buf[0] = '\0';
            for (k = 0; k < a->npins; k++) {
                int w = snprintf(buf + off, sizeof(buf) - off, "%s%u",
                                 (k ? "," : ""), (unsigned)a->pins[k]);
                if (w < 0 || (uint32_t)w >= sizeof(buf) - off) break;
                off += (uint32_t)w;
            }
            wd_res("pin %s SET %s -> %s", a->name, a->lines, buf);
        }
        return HW_WDBG_R_OK;
    }
    if (strcmp(sub, "reset") == 0) {
        uint8_t i;
        for (i = 0; i < HW_WDBG_PROTO_MAX; i++) g_pin_active[i] = g_pin_default[i];
        wd_res("pin RESET -> 默认线序");
        return HW_WDBG_R_OK;
    }
    wd_res("pin: 未知子命令 %s", sub);
    return HW_WDBG_R_BADARG;
}

static int wd_cmd_status(void)
{
    wd_res("status: %s baud=%u tx=%u rx=%u spill=%u spi=%u i2c=%u pwm=%uHz/%u ev=%u",
           hw_wdbg_mode_str(hw_wdbg_mode()), (unsigned)g_st.baud,
           (unsigned)g_st.tx_bytes, (unsigned)g_st.rx_bytes, (unsigned)g_st.spill,
           (unsigned)g_st.spi_traces, (unsigned)g_st.i2c_traces,
           (unsigned)g_st.pwm_hz, (unsigned)g_st.pwm_duty, (unsigned)g_st.events);
    return HW_WDBG_R_OK;
}

/* ============================================================
 * 10. 命令分发
 * ============================================================ */
int hw_wdbg_cmd(const char* cmd, void* ctx)
{
    int nt;
    (void)ctx;
    if (cmd == NULL) return HW_WDBG_R_NOCMD;
    nt = wd_tok(cmd);
    if (nt == 0) return HW_WDBG_R_NOCMD;
    g_res[0] = '\0';
    g_st.events++;

    if (strcmp(g_tok[0], "bridge") == 0) return wd_cmd_bridge(nt);
    if (strcmp(g_tok[0], "pwm") == 0)    return wd_cmd_pwm(nt);
    if (strcmp(g_tok[0], "spi") == 0)    return wd_cmd_spi(nt);
    if (strcmp(g_tok[0], "i2c") == 0)    return wd_cmd_i2c(nt);
    if (strcmp(g_tok[0], "pin") == 0)    return wd_cmd_pin(nt);
    if (strcmp(g_tok[0], "status") == 0) return wd_cmd_status();
    if (strcmp(g_tok[0], "count") == 0)  return (int)HW_WDBG_PROTO_MAX;
    if (strcmp(g_tok[0], "mode") == 0)   return (int)hw_wdbg_mode();
    if (strcmp(g_tok[0], "help") == 0) {
        wd_res("bridge open/close/baud/wr/rd/stat | pwm out/off/inject/meas/stat | "
               "spi mode/xfer/list/clr | i2c wr/rd/list/clr | pin def/get/set/reset | status");
        return HW_WDBG_R_HELP;
    }
    return HW_WDBG_R_NOCMD;
}

/* ============================================================
 * 11. 自检 (全跨式黄金锁定)
 * ============================================================ */
#define WDBG_CHECK(cond) do { if (!(cond)) fails++; } while (0)

int hw_wdbg_selftest(int (*putf)(const char*))
{
    int fails = 0;
    char line[180];
    char rbuf[256];
    hw_wdbg_stat_t st;
    uint32_t i;

    hw_wdbg_init(NULL);

    /* [1] 黄金校验和锁定 (默认线序表) */
    WDBG_CHECK(hw_wdbg_pin_checksum() == HW_WDBG_GOLDEN);

    /* [2] 线序表逐项 + 只读访问器 */
    WDBG_CHECK(g_pin_default[0].proto == HW_WDBG_PROTO_UART && g_pin_default[0].npins == 2u);
    WDBG_CHECK(g_pin_default[1].proto == HW_WDBG_PROTO_SPI  && g_pin_default[1].npins == 4u);
    WDBG_CHECK(g_pin_default[1].pins[3] == 10u);
    WDBG_CHECK(g_pin_default[2].proto == HW_WDBG_PROTO_I2C);
    WDBG_CHECK(g_pin_default[3].proto == HW_WDBG_PROTO_PWM && g_pin_default[3].pins[0] == 18u);
    WDBG_CHECK(strcmp(hw_wdbg_proto_name(HW_WDBG_PROTO_UART), "UART") == 0);
    WDBG_CHECK(strcmp(hw_wdbg_proto_name(9), "?") == 0);
    WDBG_CHECK(hw_wdbg_pin_active(HW_WDBG_PROTO_MAX) == NULL);

    /* [3] 波特率档位白名单 */
    WDBG_CHECK(hw_wdbg_baud_at(0) == 9600u && hw_wdbg_baud_at(3) == 921600u);
    WDBG_CHECK(hw_wdbg_baud_at(HW_WDBG_BAUD_MAX) == 0u);
    WDBG_CHECK(hw_wdbg_baud_ok(115200u) == 1 && hw_wdbg_baud_ok(12345u) == 0);

    /* [4] 未打开即写 → STATE */
    WDBG_CHECK(hw_wdbg_cmd("bridge wr 01", NULL) == HW_WDBG_R_STATE);

    /* [5] 打开 (非法档位拒) */
    WDBG_CHECK(hw_wdbg_cmd("bridge open 12345", NULL) == HW_WDBG_R_BADARG);
    WDBG_CHECK(hw_wdbg_cmd("bridge open 115200", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.baud == 115200u);

    /* [6] 读 DUT 问候 (模拟器确定性 "OK\r\n") */
    WDBG_CHECK(hw_wdbg_cmd("bridge rd 4", NULL) == HW_WDBG_R_OK);
    rbuf[0] = '\0'; hw_wdbg_result(rbuf, sizeof(rbuf));
    WDBG_CHECK(strstr(rbuf, "4f 4b 0d 0a") != NULL);

    /* [7] 写回环 (loopback) → tx 计数 + 读回 */
    WDBG_CHECK(hw_wdbg_cmd("bridge wr 41 42", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.tx_bytes == 2u);
    WDBG_CHECK(hw_wdbg_cmd("bridge rd 2", NULL) == HW_WDBG_R_OK);
    rbuf[0] = '\0'; hw_wdbg_result(rbuf, sizeof(rbuf));
    WDBG_CHECK(strstr(rbuf, "41 42") != NULL);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.rx_bytes == 6u);          /* 4 问候 + 2 回显 */

    /* [8] 波特率动态切换 */
    WDBG_CHECK(hw_wdbg_cmd("bridge baud 921600", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.baud == 921600u);

    /* [9] 关闭 → 再写回 STATE */
    WDBG_CHECK(hw_wdbg_cmd("bridge close", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("bridge wr 01", NULL) == HW_WDBG_R_STATE);

    /* [10] PWM: 无信号 STATE / 注入后读到 / out 自环 / off 复位 */
    WDBG_CHECK(hw_wdbg_cmd("pwm meas", NULL) == HW_WDBG_R_STATE);
    WDBG_CHECK(hw_wdbg_cmd("pwm inject 1000 250", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("pwm meas", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.pwm_hz == 1000u && st.pwm_duty == 250u);
    WDBG_CHECK(hw_wdbg_cmd("pwm out 20000 500", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("pwm meas", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.pwm_hz == 20000u && st.pwm_duty == 500u);
    WDBG_CHECK(hw_wdbg_cmd("pwm out 0 0", NULL) == HW_WDBG_R_BADARG);
    WDBG_CHECK(hw_wdbg_cmd("pwm off", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("pwm meas", NULL) == HW_WDBG_R_STATE);

    /* [11] SPI: Mode 校验 + 异或回读 (0xA5) + 捕获计数 */
    WDBG_CHECK(hw_wdbg_cmd("spi mode 4", NULL) == HW_WDBG_R_BADARG);
    WDBG_CHECK(hw_wdbg_cmd("spi mode 3", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("spi xfer 01 02 03", NULL) == HW_WDBG_R_OK);
    rbuf[0] = '\0'; hw_wdbg_result(rbuf, sizeof(rbuf));
    WDBG_CHECK(strstr(rbuf, "a4 a7 a6") != NULL);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.spi_traces == 1u);

    /* [12] I2C: 读递增字节 + 写 + 捕获计数 + 地址越界拒 */
    WDBG_CHECK(hw_wdbg_cmd("i2c rd 0x50 2", NULL) == HW_WDBG_R_OK);
    rbuf[0] = '\0'; hw_wdbg_result(rbuf, sizeof(rbuf));
    WDBG_CHECK(strstr(rbuf, "50 51") != NULL);
    WDBG_CHECK(hw_wdbg_cmd("i2c wr 0x50 10 20", NULL) == HW_WDBG_R_OK);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.i2c_traces == 2u);
    WDBG_CHECK(hw_wdbg_cmd("i2c rd 200 2", NULL) == HW_WDBG_R_BADARG);

    /* [13] 捕获环形缓冲溢出: 只保留 TRACE_MAX 条 (原件 50 条语义) */
    WDBG_CHECK(hw_wdbg_cmd("spi clr", NULL) == HW_WDBG_R_OK);
    for (i = 0; i < HW_WDBG_TRACE_MAX + 5u; i++)
        (void)hw_wdbg_cmd("spi xfer 00", NULL);
    WDBG_CHECK(hw_wdbg_cmd("spi list", NULL) == HW_WDBG_R_OK);
    rbuf[0] = '\0'; hw_wdbg_result(rbuf, sizeof(rbuf));
    WDBG_CHECK(strstr(rbuf, "stored=50/50") != NULL);

    /* [14] 自定义线序: set/get/reset (默认表只读不受污染) */
    WDBG_CHECK(hw_wdbg_cmd("pin set uart 5", NULL) == HW_WDBG_R_BADARG);
    WDBG_CHECK(hw_wdbg_cmd("pin set uart 5 6", NULL) == HW_WDBG_R_OK);
    {
        const hw_wdbg_pin_t* a = hw_wdbg_pin_active(HW_WDBG_PROTO_UART);
        WDBG_CHECK(a != NULL && a->pins[0] == 5u && a->pins[1] == 6u);
    }
    WDBG_CHECK(hw_wdbg_pin_checksum() == HW_WDBG_GOLDEN);
    WDBG_CHECK(hw_wdbg_cmd("pin reset", NULL) == HW_WDBG_R_OK);
    {
        const hw_wdbg_pin_t* a = hw_wdbg_pin_active(HW_WDBG_PROTO_UART);
        WDBG_CHECK(a != NULL && a->pins[0] == 47u && a->pins[1] == 21u);
    }
    WDBG_CHECK(hw_wdbg_cmd("pin set spi 1 2", NULL) == HW_WDBG_R_BADARG);

    /* [15] status / 分发错误面 */
    WDBG_CHECK(hw_wdbg_cmd("status", NULL) == HW_WDBG_R_OK);
    WDBG_CHECK(hw_wdbg_cmd("count", NULL) == (int)HW_WDBG_PROTO_MAX);
    WDBG_CHECK(hw_wdbg_cmd("mode", NULL) == (int)hw_wdbg_mode());
    WDBG_CHECK(hw_wdbg_mode() < HW_WDBG_MODE_MAX);
    WDBG_CHECK(hw_wdbg_cmd("help", NULL) == HW_WDBG_R_HELP);
    WDBG_CHECK(hw_wdbg_cmd("bogus", NULL) == HW_WDBG_R_NOCMD);
    WDBG_CHECK(hw_wdbg_cmd(NULL, NULL) == HW_WDBG_R_NOCMD);

    /* [16] init 幂等复位 (旧状态不跨运行残留) */
    hw_wdbg_init(NULL);
    hw_wdbg_stat(&st);
    WDBG_CHECK(st.baud == 0u && st.tx_bytes == 0u && st.spi_traces == 0u && st.i2c_traces == 0u);
    {
        const hw_wdbg_pin_t* a = hw_wdbg_pin_active(HW_WDBG_PROTO_SPI);
        WDBG_CHECK(a != NULL && a->pins[0] == 12u);
    }

    if (fails == 0) {
        snprintf(line, sizeof(line),
                 "hw_wdbg selftest: all PASS (golden=0x%08X, mode=%s, baud档=%u)\n",
                 (unsigned)hw_wdbg_pin_checksum(),
                 hw_wdbg_mode_str(hw_wdbg_mode()), (unsigned)HW_WDBG_BAUD_MAX);
        if (putf) putf(line);   /* putf=NULL = 静默自检 (真机固件) */
    }
    return fails;
}

/* ============================================================
 * 12. CLI: ./xiaomo wdbg [card|selftest|status|...]
 * ============================================================ */
static int wdbg_cli_puts(const char* s) { return printf("%s", s); }

static void wdbg_cli_help(void)
{
    printf("wdbg 子命令 (无线调试器信号层 / AI远程调试器复刻):\n");
    printf("  card        能力卡 (默认, 全扇区演示)\n");
    printf("  selftest    自检\n");
    printf("  任意命令    一次性分发, 例: ./xiaomo wdbg \"bridge open 115200\"\n");
    printf("  bridge      open <baud> | close | baud <baud> | wr <hex...> | rd [len] | stat\n");
    printf("  pwm         out <hz> <duty/1000> | off | inject <hz> <duty> | meas | stat\n");
    printf("  spi         mode <0..3> | xfer <hex...> | list | clr\n");
    printf("  i2c         wr <addr7> <hex...> | rd <addr7> <len> | list | clr\n");
    printf("  pin         def | get <proto> | set <proto> <pins...> | reset\n");
}

int hw_wdbg_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "card";

    /* CLI 入口统一上电复位 (一次性分发路径也须有默认线序可读) */
    hw_wdbg_init(NULL);

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_wdbg_selftest(wdbg_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "help") == 0) { wdbg_cli_help(); return 0; }

    if (strcmp(sub, "card") == 0) {
        char rbuf[256];
        uint8_t i;
        hw_wdbg_init(NULL);
        printf("=== xiaomo hw_wdbg 无线调试器信号层 ===\n");
        printf("复刻: 立创开源「AI远程调试器」(ESP32-S3 无线串口调试器)\n");
        printf("mode      = %s (%u)\n", hw_wdbg_mode_str(hw_wdbg_mode()), (unsigned)hw_wdbg_mode());
        {
            uint32_t ck = hw_wdbg_pin_checksum();
            printf("pin cksum = 0x%08X %s\n", (unsigned)ck,
                   (ck == HW_WDBG_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
        }
        printf("---- 默认线序 (%u 协议) ----\n", (unsigned)HW_WDBG_PROTO_MAX);
        for (i = 0; i < HW_WDBG_PROTO_MAX; i++) {
            uint32_t k;
            printf("  %-4s %-18s rate=%-8u pins:", g_pin_default[i].name,
                   g_pin_default[i].lines, (unsigned)g_pin_default[i].rate);
            for (k = 0; k < g_pin_default[i].npins; k++)
                printf(" %u", (unsigned)g_pin_default[i].pins[k]);
            printf("\n");
        }
        printf("---- 串口桥接 (2048B 流缓冲) ----\n");
        (void)hw_wdbg_cmd("bridge open 115200", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  open  -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("bridge rd 4", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  rd    -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("bridge wr 48 49", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  wr    -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("bridge rd 2", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  rd    -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("bridge stat", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  stat  -> [%s]\n", rbuf);
        printf("---- 多协议信号监控 ----\n");
        (void)hw_wdbg_cmd("pwm inject 1000 250", NULL);
        (void)hw_wdbg_cmd("pwm meas", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  pwm   -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("spi mode 3", NULL);
        (void)hw_wdbg_cmd("spi xfer 01 02 03", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  spi   -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("i2c rd 0x50 4", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  i2c   -> [%s]\n", rbuf);
        (void)hw_wdbg_cmd("status", NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf)); printf("  stat  -> [%s]\n", rbuf);
        printf("VM 内核联动: kvm_run 上电自动 hw_wdbg_init; .mo 端 hw_wdbg(\"...\")\n");
        printf("  见 examples/wdbg_test.mo\n");
        return 0;
    }

    /* 其余 = 一次性分发 (把 argv[2..] 拼成一条命令) */
    {
        char joined[256];
        char rbuf[256];
        int rc, off = 0;
        int i;
        joined[0] = '\0';
        for (i = 2; i < argc; i++) {
            int w = snprintf(joined + off, sizeof(joined) - (size_t)off, "%s%s",
                             (i > 2 ? " " : ""), argv[i]);
            if (w < 0 || (uint32_t)w >= sizeof(joined) - (uint32_t)off) break;
            off += w;
        }
        rc = hw_wdbg_cmd(joined, NULL);
        hw_wdbg_result(rbuf, sizeof(rbuf));
        if (rbuf[0] != '\0')
            printf("wdbg rc=%d (%s) result=[%s]\n", rc, hw_wdbg_result_code_str(rc), rbuf);
        else
            printf("wdbg rc=%d (%s)\n", rc, hw_wdbg_result_code_str(rc));
        return (rc >= 0) ? 0 : 1;
    }
}
