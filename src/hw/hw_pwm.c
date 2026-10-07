/* ============================================================================
 * xiaomo - PWM 脉冲输出层 (hw_pwm) —— 实现
 * ============================================================================
 * 依赖纪律 (家族规范):
 *   - 只用标准函数; 无 malloc / 无 pthread / 无文件 IO
 *   - 硬件差异一律走 BSP 回调注入; 默认 = 表驱动, 六模式行为一致可回归
 *   - 大局部数组一律 static (真机栈纪律, 坑 #12)
 *   - 局部变量名避开 C++ 保留字 (坑 #19: try/class/new/bool/...)
 *   - extern "C" 包全文件 (坑 #44: 局部守卫会让 C++ 形态链接炸)
 *   - init() 只复位模块自身状态, 绝不碰 BSP (坑 #25: 上电自动 init 会静默
 *     抹掉真机装好的 BSP, 是家族头号假成功)
 *   - 黄金值断言打在【公共出口】上, 不是 selftest 内部重算 (坑 #28)
 * ==========================================================================*/
#include "hw_pwm.h"
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. 档案表 (数据, 与头文件真值逐条同源; 黄金值钉死)
 * ============================================================ */
static const uint32_t k_reg_addr[PWM_REG_MAX] = {
    HW_PWM_DC, HW_PWM_DDC, HW_PWM_OEP, HW_PWM_EMC,
    HW_PWM_OPEN, HW_PWM_PUT, HW_PWM_POST
};

static const char* const k_reg_name[PWM_REG_MAX] = {
    "DC", "DDC", "OEP", "EMC", "OPEN", "PUT", "POST"
};

static const uint32_t k_pin_addr[PWM_PIN_MAX] = { HW_PWM_VPP, HW_PWM_GND };
static const char* const k_pin_name[PWM_PIN_MAX] = { "VPP", "GND" };

static const char* const k_mode_name[HW_PWM_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

/* ============================================================
 * 2. 编译期模式探测 (全跨模式识别, 与 hw_token 同款优先级)
 * ============================================================ */
static uint8_t pwm_probe_mode(void)
{
#if defined(HW_PWM_MODE_OVERRIDE)
    return (uint8_t)HW_PWM_MODE_OVERRIDE;
#elif defined(HW_PWM_KELL)
    /* KELL 内核里也是 __linux__, 须由 KELL 构建系统加 -DHW_PWM_KELL=1 抢先 */
    return (uint8_t)HW_PWM_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_PWM_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_PWM_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_PWM_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_PWM_MODE_HOST;
#else
    return (uint8_t)HW_PWM_MODE_TEST;
#endif
}

uint8_t pwm_mode(void)
{
    static uint8_t cached = 0xFFu;      /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = pwm_probe_mode();
    return cached;
}

const char* pwm_mode_str(uint8_t mode)
{
    if ((uint32_t)mode >= (uint32_t)HW_PWM_MODE_MAX) return "??";
    return k_mode_name[mode];
}

/* ============================================================
 * 3. FNV-1a-32 (口径公开, 与 Python 侧同款)
 * ============================================================ */
#define PWM_FNV_OFF 2166136261u
#define PWM_FNV_PRM 16777619u

uint32_t pwm_fnv32_feed_byte(uint32_t h, uint8_t b)
{
    h ^= (uint32_t)b;
    h *= PWM_FNV_PRM;
    return h;
}

uint32_t pwm_fnv32(const void* data, uint32_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = PWM_FNV_OFF;
    uint32_t i;
    if (p == NULL) return 0u;
    for (i = 0u; i < n; i++) h = pwm_fnv32_feed_byte(h, p[i]);
    return h;
}

/* 把 uint32 按【小端 4 字节】喂进 FNV —— 黄金值口径的一半。
 * ⚠️ 端序必须写死小端: 大端机上逐字节喂的顺序不同, 黄金值就裂了。 */
static uint32_t pwm_fnv32_u32(uint32_t h, uint32_t v)
{
    h = pwm_fnv32_feed_byte(h, (uint8_t)(v & 0xFFu));
    h = pwm_fnv32_feed_byte(h, (uint8_t)((v >> 8)  & 0xFFu));
    h = pwm_fnv32_feed_byte(h, (uint8_t)((v >> 16) & 0xFFu));
    h = pwm_fnv32_feed_byte(h, (uint8_t)((v >> 24) & 0xFFu));
    return h;
}

/* ============================================================
 * 4. BSP 注入
 * ============================================================ */
static pwm_reg_read_fn  g_read;
static pwm_reg_write_fn g_write;
static void*            g_user;

void pwm_bsp_install(pwm_reg_read_fn rd, pwm_reg_write_fn wr, void* user)
{
    g_read  = rd;
    g_write = wr;
    g_user  = user;
}

void pwm_bsp_uninstall(void)
{
    g_read  = NULL;
    g_write = NULL;
    g_user  = NULL;
}

int pwm_bsp_installed(void) { return (g_read != NULL) ? 1 : 0; }

/* SIM/REAL 的结构性分水岭: g_read == NULL 即表驱动 (家族坑 #21) */
uint32_t pwm_reg_read(uint32_t addr)
{
    if (g_read == NULL) return 0u;     /* 无硬件: 诚实 0, 不碰野地址 */
    return g_read(addr, g_user);
}

int pwm_reg_write(uint32_t addr, uint32_t val)
{
    if (g_write == NULL) return PWM_ERR_NOBSP;   /* 无硬件: 诚实拒绝, 不假装写成功 */
    return g_write(addr, val, g_user);
}

/* ---- 初版两个 inline 的正式实现 (语义照搬) ---- */
uint32_t pwm_reg_read_oep_scaled(void)
{
    uint32_t val = pwm_reg_read(HW_PWM_OEP);
    return val << 5U;                 /* 初版: return pwm_crw(0x063800U) << 5U */
}

int pwm_reg_map_emc_to_ddc(void)
{
    uint32_t emc_val = pwm_reg_read(HW_PWM_EMC);
    return pwm_reg_write(HW_PWM_DDC, emc_val);   /* 初版: pwm_cww(DDC, emc_val) */
}

/* ============================================================
 * 5. 档案查询
 * ============================================================ */
const char* pwm_reg_name(uint32_t idx)
{
    if (idx >= (uint32_t)PWM_REG_MAX) return "?";   /* 先校验后索引 (坑 #31) */
    return k_reg_name[idx];
}

int pwm_reg_addr(uint32_t idx, uint32_t* out)
{
    if (out == NULL) return PWM_ERR_PARAM;
    if (idx >= (uint32_t)PWM_REG_MAX) return PWM_ERR_BOUND;
    *out = k_reg_addr[idx];
    return PWM_OK;
}

int pwm_reg_index(uint32_t addr)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) {
        if (k_reg_addr[i] == addr) return (int)i;
    }
    return -1;
}

int pwm_pin_addr(uint32_t idx, uint32_t* out)
{
    if (out == NULL) return PWM_ERR_PARAM;
    if (idx >= (uint32_t)PWM_PIN_MAX) return PWM_ERR_BOUND;
    *out = k_pin_addr[idx];
    return PWM_OK;
}

const char* pwm_pin_name(uint32_t idx)
{
    if (idx >= (uint32_t)PWM_PIN_MAX) return "?";
    return k_pin_name[idx];
}

/* ---- 黄金值: 复算 + 公共出口 ----
 * 顺序纪律: raw 复算出口先定义, 掩码导出后定义 —— 断言打在【公共出口】,
 * 复算打在 raw 出口, 两者分开才抓得到"selftest 全绿而 CLI 全错" (坑 #28)。 */
uint32_t pwm_fnv32_reg_raw(void)
{
    uint32_t h = PWM_FNV_OFF;
    uint32_t i;
    for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) h = pwm_fnv32_u32(h, k_reg_addr[i]);
    return h;
}

uint32_t pwm_fnv32_pin_raw(void)
{
    uint32_t h = PWM_FNV_OFF;
    uint32_t i;
    for (i = 0u; i < (uint32_t)PWM_PIN_MAX; i++) h = pwm_fnv32_u32(h, k_pin_addr[i]);
    return h;
}

uint32_t pwm_fnv32_vec_raw(void)
{
    static const uint32_t k_vec_pct[5] = { 0u, 25u, 50u, 75u, 100u };
    uint32_t h = PWM_FNV_OFF;
    uint32_t i;
    for (i = 0u; i < 5u; i++) {
        uint32_t code = 0u;
        (void)pwm_duty_encode(k_vec_pct[i], &code);
        h = pwm_fnv32_u32(h, code);
    }
    return h;
}

uint32_t pwm_golden_raw(void)
{
    uint32_t h = PWM_FNV_OFF;
    uint32_t i;
    for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) h = pwm_fnv32_u32(h, k_reg_addr[i]);
    for (i = 0u; i < (uint32_t)PWM_PIN_MAX; i++) h = pwm_fnv32_u32(h, k_pin_addr[i]);
    h = pwm_fnv32_u32(h, HW_PWM_FREQUENCY);
    return h;
}

uint32_t pwm_profile_golden(void) { return (pwm_golden_raw()  & PWM_OUT_MASK); }
uint32_t pwm_golden_reg(void)     { return (pwm_fnv32_reg_raw() & PWM_OUT_MASK); }
uint32_t pwm_golden_vec(void)     { return (pwm_fnv32_vec_raw() & PWM_OUT_MASK); }

/* ============================================================
 * 6. 频率与占空比 (纯算术)
 * ============================================================ */
int pwm_duty_encode(uint32_t pct, uint32_t* out)
{
    if (out == NULL) return PWM_ERR_PARAM;
    if (pct > (uint32_t)PWM_DUTY_MAX) return PWM_ERR_PARAM;   /* 先校验后算 */
    /* 四舍五入: (pct*65535 + 50) / 100 —— 与 Python 侧 ((p*65535+50)//100) 同款 */
    *out = (pct * (uint32_t)PWM_DUTY_SCALE + 50u) / 100u;
    return PWM_OK;
}

int pwm_duty_decode(uint32_t code, uint32_t* pct)
{
    if (pct == NULL) return PWM_ERR_PARAM;
    if (code > (uint32_t)PWM_DUTY_SCALE) return PWM_ERR_PARAM;
    *pct = (code * 100u + (uint32_t)(PWM_DUTY_SCALE / 2u)) / (uint32_t)PWM_DUTY_SCALE;
    return PWM_OK;
}

int pwm_freq_valid(uint32_t hz)
{
    if (hz == 0u) return 0;                 /* 0 Hz 无意义 */
    if (hz > (uint32_t)PWM_FREQ_MAX) return 0;
    return 1;
}

/* ============================================================
 * 7. 通道状态机
 *    【init 只复位自身状态, 绝不 memset BSP】(坑 #25)
 * ============================================================ */
static pwm_ch_t g_ch[PWM_CH_MAX];

int pwm_ch_init(void)
{
    uint32_t i;
    for (i = 0u; i < (uint32_t)PWM_CH_MAX; i++) {
        g_ch[i].ch        = i;
        g_ch[i].hz        = 0u;
        g_ch[i].duty_pct  = 0u;
        g_ch[i].duty_code = 0u;
        g_ch[i].running   = 0u;
    }
    return PWM_OK;
}

int pwm_ch_config(uint32_t ch, uint32_t hz, uint32_t pct)
{
    uint32_t code = 0u;
    if (ch >= (uint32_t)PWM_CH_MAX) return PWM_ERR_BOUND;
    if (pct > (uint32_t)PWM_DUTY_MAX) return PWM_ERR_PARAM;
    if (pwm_freq_valid(hz) == 0) return PWM_ERR_PARAM;
    if (pwm_duty_encode(pct, &code) != PWM_OK) return PWM_ERR_PARAM;
    g_ch[ch].hz        = hz;
    g_ch[ch].duty_pct  = pct;
    g_ch[ch].duty_code = code;      /* duty_code 恒为 duty_pct 的派生, 不独立存 */
    return PWM_OK;
}

int pwm_ch_start(uint32_t ch)
{
    if (ch >= (uint32_t)PWM_CH_MAX) return PWM_ERR_BOUND;
    /* 真正去驱动输出 = 需要硬件。无 BSP 时【不】置 running:
     * 诚实失败 (没真驱动就不说自己在跑), 家族头号假成功的反面纪律。 */
    if (pwm_reg_write(HW_PWM_OPEN, 1u) != PWM_OK) return PWM_ERR_NOBSP;
    g_ch[ch].running = 1u;
    return PWM_OK;
}

int pwm_ch_stop(uint32_t ch)
{
    if (ch >= (uint32_t)PWM_CH_MAX) return PWM_ERR_BOUND;
    g_ch[ch].running = 0u;
    return PWM_OK;
}

int pwm_ch_get(uint32_t ch, pwm_ch_t* out)
{
    if (out == NULL) return PWM_ERR_PARAM;
    if (ch >= (uint32_t)PWM_CH_MAX) return PWM_ERR_BOUND;
    *out = g_ch[ch];
    return PWM_OK;
}

int pwm_ch_count(void) { return (int)PWM_CH_MAX; }

/* ============================================================
 * 8. 命令通道 (VM / CLI 共用)
 * ============================================================ */

/* 严格解析十进制无符号串 —— 【带长度】。
 * ⚠️ 踩过的坑: token 是空格分隔的【子串】, 后面还跟着别的参数。
 *    只靠 NUL 终止的解析器会把 "1 50 1000" 里的空格判成非法字符,
 *    于是只有【最后一个】参数恰好 NUL 结尾时才解析成功 —— 症状是
 *    "两参数能跑, 三参数报参数错", 极难查。纪律 = 不可信长度一律带长度传。
 * 整体必须恰好消费 len 个字符, 多一个少一个都判负。 */
static int pwm_parse_u32n(const char* s, uint32_t len, uint32_t* out)
{
    uint32_t v = 0u;
    uint32_t i;
    if (s == NULL || out == NULL) return PWM_ERR_PARAM;
    if (len == 0u || len > 10u) return PWM_ERR_PARAM;
    for (i = 0u; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return PWM_ERR_PARAM;
        if (v > (0xFFFFFFFFu - (uint32_t)(s[i] - '0')) / 10u) return PWM_ERR_PARAM;
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return PWM_OK;
}

/* 取下一个空格分隔的 token; 返回 NULL 表示没有了。
 * 不修改入参 —— 家族坑 #41 的教训: 别在只读缓冲上做破坏性切分。
 * ⚠️ 返回的 token 是【子串】不是 C 字符串: 后面还跟着别的参数,
 *    消费方必须连 toklen 一起用 (见 pwm_parse_u32n)。 */
static int pwm_next_tok(const char** cur, const char** tok, uint32_t* toklen)
{
    const char* p = *cur;
    const char* start;
    if (p == NULL) return PWM_ERR_PARAM;
    while (*p == ' ') p++;
    if (*p == '\0') return PWM_ERR_REJECT;
    start = p;
    while (*p != '\0' && *p != ' ') p++;
    *tok    = start;
    *toklen = (uint32_t)(p - start);
    *cur    = p;
    return PWM_OK;
}

/* 便捷包装: 取一个 u32 参数 */
static int pwm_arg_u32(const char** cur, uint32_t* out)
{
    const char* t;
    uint32_t    n;
    if (pwm_next_tok(cur, &t, &n) != PWM_OK) return PWM_ERR_PARAM;
    return pwm_parse_u32n(t, n, out);
}

/* 命令通道返回值契约 (对齐家族基准 hw_dc_cmd, 别自己发明):
 *     查询命令  → 返回【值】(非负)。0 是合法值, 不是"出错"。
 *     改动命令  → 0 = 成功, 负数 = 错误码。
 *     未知命令  → PWM_ERR_REJECT。
 * ⚠️ 踩过的坑: 初版让 reg <idx> 返回 pwm_reg_addr() 的【返回码】(恒 0),
 *    地址写进一个没人读的局部变量 —— 查询命令静默返回 0。
 *    症状 = "reg 3 跑通了但答 0", 看起来像寄存器全是 0。
 *    判别法 = 查询命令的返回码必须和 API 的【值出口】逐位一致 (坑 #28)。 */
int pwm_cmd(const char* cmd)
{
    const char* cur = cmd;
    const char* tok;
    uint32_t    toklen;
    uint32_t    a = 0u, b = 0u, c = 0u;
    int        rc;

    if (cmd == NULL) return PWM_ERR_PARAM;

    if (pwm_next_tok(&cur, &tok, &toklen) != PWM_OK) return PWM_ERR_REJECT;

    /* --- 无参命令 --- */
    if (toklen == 4u && strncmp(tok, "init", 4u) == 0) {
        return pwm_ch_init();                            /* 改动: 0 = 成功 */
    }
    if (toklen == 4u && strncmp(tok, "mode", 4u) == 0) {
        return (int)pwm_mode();                          /* 查询: 值 */
    }
    if (toklen == 6u && strncmp(tok, "golden", 6u) == 0) {
        return (int)pwm_profile_golden();                /* 查询: 公共出口 */
    }
    if (toklen == 6u && strncmp(tok, "regcnt", 6u) == 0) {
        return (int)PWM_REG_MAX;                         /* 查询: 值 */
    }
    if (toklen == 6u && strncmp(tok, "pincnt", 6u) == 0) {
        return (int)PWM_PIN_MAX;                         /* 查询: 值 */
    }
    if (toklen == 3u && strncmp(tok, "bsp", 3u) == 0) {
        return pwm_bsp_installed();                     /* 查询: 0/1 */
    }

    /* --- reg <idx>: 取【地址值】(不是取返回码) --- */
    if (toklen == 3u && strncmp(tok, "reg", 3u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        rc = pwm_reg_addr(a, &b);
        if (rc != PWM_OK) return rc;                     /* 越界原样透出 */
        return (int)b;                                   /* ← 值出口 */
    }

    /* --- regidx <addr>: 反查下标 --- */
    if (toklen == 6u && strncmp(tok, "regidx", 6u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        return pwm_reg_index(a);                         /* 未命中 = -1 */
    }

    /* --- ch <n> <pct> <hz> --- */
    if (toklen == 2u && strncmp(tok, "ch", 2u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;   /* n   */
        if (pwm_arg_u32(&cur, &b) != PWM_OK) return PWM_ERR_PARAM;   /* pct */
        if (pwm_arg_u32(&cur, &c) != PWM_OK) return PWM_ERR_PARAM;   /* hz  */
        return pwm_ch_config(a, c, b);        /* 注意实参序: (ch, hz, pct) */
    }

    /* --- get <n>: 回读通道, 返回 running 标志 (查询) --- */
    if (toklen == 3u && strncmp(tok, "get", 3u) == 0) {
        pwm_ch_t st;
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        rc = pwm_ch_get(a, &st);
        if (rc != PWM_OK) return rc;
        return (int)st.running;
    }

    /* --- duty <pct>: 纯换算, 无硬件 (查询) --- */
    if (toklen == 4u && strncmp(tok, "duty", 4u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        rc = pwm_duty_encode(a, &b);
        if (rc != PWM_OK) return rc;
        return (int)b;
    }

    /* --- start <n> / stop <n> --- */
    if (toklen == 5u && strncmp(tok, "start", 5u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        return pwm_ch_start(a);
    }
    if (toklen == 4u && strncmp(tok, "stop", 4u) == 0) {
        if (pwm_arg_u32(&cur, &a) != PWM_OK) return PWM_ERR_PARAM;
        return pwm_ch_stop(a);
    }

    /* --- oep: 初版 OEP 缩放读 (公开出口, 便于真机核对) --- */
    if (toklen == 3u && strncmp(tok, "oep", 3u) == 0) {
        return (int)pwm_reg_read_oep_scaled();
    }

    return PWM_ERR_REJECT;   /* 未知命令 */
}

/* ============================================================
 * 9. 自检
 * ============================================================ */

/* ---- 测试用桩: 记录最后一次写, 供 REAL 档断言 ---- */
typedef struct {
    uint32_t last_addr;
    uint32_t last_val;
    uint32_t rd_val;      /* 读回固定值 */
    uint32_t wr_count;
} pwm_stub_t;

static pwm_stub_t g_stub;

static uint32_t pwm_stub_read(uint32_t addr, void* user)
{
    (void)addr; (void)user;
    return g_stub.rd_val;
}

static int pwm_stub_write(uint32_t addr, uint32_t val, void* user)
{
    (void)user;
    g_stub.last_addr = addr;
    g_stub.last_val  = val;
    g_stub.wr_count++;
    return PWM_OK;
}

static int pwm_puts_nop(const char* s) { (void)s; return 0; }

/* 变异纪律: checks 必须【真累加】。早先这里是硬编码字符串 "ALL PASS (34 checks)",
 * 于是加断言时数字纹丝不动 —— 一个不具区分力的信号, 比没有信号更险 (它让人
 * "有依据地"相信覆盖在增长)。所以每条 PWM_CHECK 都自增 checks, 末尾打印实数。 */
#define PWM_CHECK(n, cond, why)                                        \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            fails++;                                                   \
            if (putf != NULL) {                                        \
                char m[96];                                            \
                (void)snprintf(m, sizeof(m),                          \
                               "[FAIL] [%s] %s\n", (n), (why));       \
                putf(m);                                               \
            }                                                          \
        }                                                              \
    } while (0)

int pwm_selftest(pwm_puts_fn putf)
{
    int      fails  = 0;
    uint32_t checks = 0u;
    uint32_t i, a;
    uint32_t code, pct;
    pwm_ch_t ch;
    /* 【开头保存 BSP】—— 真机自检与真机扫描不得互相污染 (坑 #25) */
    pwm_reg_read_fn  sv_rd  = g_read;
    pwm_reg_write_fn sv_wr  = g_write;
    void*            sv_usr = g_user;

    if (putf == NULL) putf = pwm_puts_nop;

    /* ---- [1..3] 档案锁的三条编译期断言, 运行期再钉一次 ---- */
    PWM_CHECK("1", HW_PWM_FREQUENCY <= (uint32_t)PWM_FREQ_MAX, "freq over max");
    PWM_CHECK("2", HW_PWM_DC <= 0x033000U, "DC out of range");
    PWM_CHECK("3", HW_PWM_DDC == 0x052800U, "DDC must be 0x052800");

    /* ---- [4] 寄存器表 7 项地址 == 用户真值 (逐条) ---- */
    {
        static const uint32_t k_expect[PWM_REG_MAX] = {
            0x030000u, 0x052800u, 0x063800u, 0x103000u,
            0x010358u, 0x0EFAFFu, 0x0DEEFFu
        };
        for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) {
            PWM_CHECK("4", pwm_reg_addr(i, &a) == PWM_OK && a == k_expect[i],
                      "reg addr mismatch");
        }
    }

    /* ---- [5] 寄存器名表: 逐条比对【具体名字】 ----
     * 变异 M15 实锤: 原判据只有 strcmp(..., "?") != 0, 把名字表打乱
     * (DC<->DDC) 照样全绿 —— 弱判据不具区分力。必须比对字面量本身。 */
    {
        static const char* const k_name_expect[PWM_REG_MAX] = {
            "DC", "DDC", "OEP", "EMC", "OPEN", "PUT", "POST"
        };
        for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) {
            const char* n = pwm_reg_name(i);
            PWM_CHECK("5", n != NULL && strcmp(n, k_name_expect[i]) == 0,
                      "reg name mismatch (weak compare caught nothing)");
        }
    }

    /* ---- [6] 越界索引必须判负, 不得静默取到表内 (坑 #31) ---- */
    PWM_CHECK("6", pwm_reg_addr(PWM_REG_MAX, &a) == PWM_ERR_BOUND, "reg oob not rejected");
    PWM_CHECK("6", pwm_reg_name(PWM_REG_MAX)[0] == '?', "reg name oob not rejected");

    /* ---- [7] 地址反查: 每项命中自身, 未知地址判负 ---- */
    for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) {
        PWM_CHECK("7", pwm_reg_addr(i, &a) == PWM_OK && pwm_reg_index(a) == (int)i,
                  "reg index roundtrip");
    }
    PWM_CHECK("7", pwm_reg_index(0xDEADBEEFu) == -1, "unknown addr not rejected");

    /* ---- [8][9] 脚号表 ---- */
    PWM_CHECK("8", pwm_pin_addr(0u, &a) == PWM_OK && a == HW_PWM_VPP, "VPP mismatch");
    PWM_CHECK("8", pwm_pin_addr(1u, &a) == PWM_OK && a == HW_PWM_GND, "GND mismatch");
    PWM_CHECK("9", pwm_pin_addr(PWM_PIN_MAX, &a) == PWM_ERR_BOUND, "pin oob not rejected");
    /* 变异 M11 实锤: 原来只测了 pwm_pin_addr 的越界, pwm_pin_name 的越界
     * 守卫【一次都没被测过】 ⇒ 把 >= 改成 > 仍然全绿。名字与地址必须同守。 */
    PWM_CHECK("9", pwm_pin_name(PWM_PIN_MAX)[0] == '?', "pin NAME oob not rejected");
    PWM_CHECK("9", pwm_pin_name(PWM_PIN_MAX + 1u)[0] == '?', "pin NAME oob+1 not rejected");
    /* 名字也要比对字面量, 不能只测 != "?" (同 M15 的弱判据教训) */
    PWM_CHECK("9", strcmp(pwm_pin_name(0u), "VPP") == 0, "pin name 0 mismatch");
    PWM_CHECK("9", strcmp(pwm_pin_name(1u), "GND") == 0, "pin name 1 mismatch");

    /* ---- [10..13] 四个黄金值复算 == 硬编码 ---- */
    PWM_CHECK("10", pwm_fnv32_reg_raw() == PWM_GOLDEN_REG, "golden REG mismatch");
    PWM_CHECK("11", pwm_fnv32_pin_raw() == PWM_GOLDEN_PIN, "golden PIN mismatch");
    PWM_CHECK("12", pwm_golden_raw() == PWM_GOLDEN_ALL, "golden ALL mismatch");
    PWM_CHECK("13", pwm_fnv32_vec_raw() == PWM_GOLDEN_VEC, "golden VEC mismatch");

    /* ---- [14][15] 跨 2^31 的黄金: 掩码真的生效 (坑 #48) ---- */
    PWM_CHECK("14", (PWM_GOLDEN_REG & 0x80000000u) != 0u, "REG should cross 2^31");
    PWM_CHECK("14", pwm_golden_reg() == (PWM_GOLDEN_REG & PWM_OUT_MASK),
              "golden REG exit mask wrong");
    PWM_CHECK("14", pwm_golden_reg() != PWM_GOLDEN_REG, "REG mask should be active");
    PWM_CHECK("15", (PWM_GOLDEN_VEC & 0x80000000u) != 0u, "VEC should cross 2^31");
    PWM_CHECK("15", pwm_golden_vec() == (PWM_GOLDEN_VEC & PWM_OUT_MASK),
              "golden VEC exit mask wrong");
    PWM_CHECK("15", pwm_golden_vec() != PWM_GOLDEN_VEC, "VEC mask should be active");

    /* ---- [16] 未跨 2^31 的黄金: 掩码在此【是空操作】, 两种情形都钉 ---- */
    PWM_CHECK("16", (PWM_GOLDEN_ALL & 0x80000000u) == 0u, "ALL should NOT cross 2^31");
    PWM_CHECK("16", pwm_profile_golden() == (PWM_GOLDEN_ALL & PWM_OUT_MASK),
              "golden ALL exit mask wrong");
    PWM_CHECK("16", pwm_profile_golden() == PWM_GOLDEN_ALL, "ALL mask should be inert");

    /* ---- [17] 占空比编码向量 5 个值 ---- */
    {
        static const uint32_t k_pct[5]  = { 0u, 25u, 50u, 75u, 100u };
        static const uint32_t k_code[5] = { 0u, 16384u, 32768u, 49151u, 65535u };
        for (i = 0u; i < 5u; i++) {
            code = 0xDEADu;
            PWM_CHECK("17", pwm_duty_encode(k_pct[i], &code) == PWM_OK &&
                           code == k_code[i], "duty encode vector");
        }
    }

    /* ---- [18] 编码边界 ---- */
    PWM_CHECK("18", pwm_duty_encode(0u, &code) == PWM_OK && code == 0u, "duty 0");
    PWM_CHECK("18", pwm_duty_encode(100u, &code) == PWM_OK && code == 65535u, "duty 100");
    PWM_CHECK("18", pwm_duty_encode(101u, &code) == PWM_ERR_PARAM, "duty 101 must reject");
    PWM_CHECK("18", pwm_duty_encode(0u, NULL) == PWM_ERR_PARAM, "duty NULL must reject");

    /* ---- [19] 解码往返 ---- */
    {
        static const uint32_t k_pct[5] = { 0u, 25u, 50u, 75u, 100u };
        for (i = 0u; i < 5u; i++) {
            pct = 0xDEADu;
            PWM_CHECK("19", pwm_duty_encode(k_pct[i], &code) == PWM_OK &&
                           pwm_duty_decode(code, &pct) == PWM_OK &&
                           pct == k_pct[i], "duty roundtrip");
        }
    }

    /* ---- [20] 解码边界 ---- */
    PWM_CHECK("20", pwm_duty_decode(65535u, &pct) == PWM_OK && pct == 100u, "decode full");
    PWM_CHECK("20", pwm_duty_decode(65536u, &pct) == PWM_ERR_PARAM, "decode oob must reject");

    /* ---- [21] 频率校验 ---- */
    PWM_CHECK("21", pwm_freq_valid(HW_PWM_FREQUENCY) == 1, "freq 100k must be valid");
    PWM_CHECK("21", pwm_freq_valid(0u) == 0, "freq 0 must reject");
    PWM_CHECK("21", pwm_freq_valid((uint32_t)PWM_FREQ_MAX) == 1, "freq max valid");
    PWM_CHECK("21", pwm_freq_valid((uint32_t)PWM_FREQ_MAX + 1u) == 0, "freq over max reject");

    /* ---- [22] init-keeps-bsp 回归护栏 (坑 #25: 家族头号假成功) ---- */
    g_stub.rd_val = 0x11u; g_stub.wr_count = 0u;
    pwm_bsp_install(pwm_stub_read, pwm_stub_write, NULL);
    (void)pwm_ch_init();                       /* init 不该抹掉 BSP */
    PWM_CHECK("22", pwm_bsp_installed() == 1, "init wiped BSP");
    PWM_CHECK("22", pwm_reg_read(HW_PWM_DC) == 0x11u, "after init, BSP not used");

    /* ---- [23] SIM 档: 无 BSP 时诚实 0 / 拒绝写 ---- */
    pwm_bsp_uninstall();
    PWM_CHECK("23", pwm_bsp_installed() == 0, "uninstall failed");
    PWM_CHECK("23", pwm_reg_read(HW_PWM_DC) == 0u, "SIM read must be honest 0");
    PWM_CHECK("23", pwm_reg_write(HW_PWM_DC, 1u) == PWM_ERR_NOBSP, "SIM write must reject");
    PWM_CHECK("23", pwm_reg_map_emc_to_ddc() == PWM_ERR_NOBSP, "SIM emc2ddc must reject");

    /* ---- [24] REAL 档: 桩收到正确的地址与值 ---- */
    g_stub.rd_val = 0x5Au; g_stub.wr_count = 0u;
    pwm_bsp_install(pwm_stub_read, pwm_stub_write, NULL);
    PWM_CHECK("24", pwm_reg_map_emc_to_ddc() == PWM_OK, "REAL emc2ddc");
    PWM_CHECK("24", g_stub.last_addr == HW_PWM_DDC, "emc2ddc wrong target addr");
    PWM_CHECK("24", g_stub.last_val == 0x5Au, "emc2ddc wrong value");
    /* 初版语义: OEP 读出后左移 5 位 */
    g_stub.rd_val = 0x3u;
    PWM_CHECK("24", pwm_reg_read_oep_scaled() == (0x3u << 5u), "oep scaled wrong");

    /* ---- [25] 通道配置参数校验 ---- */
    pwm_bsp_uninstall();
    (void)pwm_ch_init();
    PWM_CHECK("25", pwm_ch_config(PWM_CH_MAX, 1000u, 50u) == PWM_ERR_BOUND, "ch oob");
    PWM_CHECK("25", pwm_ch_config(0u, 1000u, 101u) == PWM_ERR_PARAM, "pct oob");
    PWM_CHECK("25", pwm_ch_config(0u, 0u, 50u) == PWM_ERR_PARAM, "hz 0");
    PWM_CHECK("25", pwm_ch_config(0u, 1000u, 50u) == PWM_OK, "valid config");

    /* ---- [26] 通道状态机: duty_code 恒为 duty_pct 的派生 ---- */
    PWM_CHECK("26", pwm_ch_get(0u, &ch) == PWM_OK, "ch get");
    PWM_CHECK("26", ch.hz == 1000u && ch.duty_pct == 50u, "ch fields");
    PWM_CHECK("26", ch.duty_code == 32768u, "duty_code not derived from pct");
    PWM_CHECK("26", ch.running == 0u, "should not be running before start");

    /* ---- [27] 通道 get 边界 ---- */
    PWM_CHECK("27", pwm_ch_get(PWM_CH_MAX, &ch) == PWM_ERR_BOUND, "ch get oob");
    PWM_CHECK("27", pwm_ch_get(0u, NULL) == PWM_ERR_PARAM, "ch get NULL");
    PWM_CHECK("28", pwm_ch_count() == (int)PWM_CH_MAX, "ch count");
    /* 🕳️ 变异 M10 实锤存活: 上面那条是【自算恒等】(pwm_ch_count() 的实现就是
     * return (int)PWM_CH_MAX), 把 PWM_CH_MAX 从 4 改成 5 它照样成立。
     * 必须有一条【绝对值】锚定 4 这个真实建模上限。 */
    PWM_CHECK("28", pwm_ch_count() == 4, "channel count must stay 4 (absolute, not self-derived)");

    /* ---- [29] 无 BSP 时 start 必须【不】置 running (诚实失败) ---- */
    PWM_CHECK("29", pwm_ch_start(0u) == PWM_ERR_NOBSP, "start without BSP must reject");
    PWM_CHECK("29", pwm_ch_get(0u, &ch) == PWM_OK && ch.running == 0u,
              "start claimed running without hardware");

    /* ---- [30] 装了 BSP 后 start 才真置 running ---- */
    g_stub.rd_val = 0u; g_stub.wr_count = 0u;
    pwm_bsp_install(pwm_stub_read, pwm_stub_write, NULL);
    PWM_CHECK("30", pwm_ch_start(0u) == PWM_OK, "start with BSP");
    PWM_CHECK("30", g_stub.last_addr == HW_PWM_OPEN, "start wrote wrong addr");
    PWM_CHECK("30", pwm_ch_get(0u, &ch) == PWM_OK && ch.running == 1u, "start not recorded");
    PWM_CHECK("30", pwm_ch_stop(0u) == PWM_OK, "stop");
    PWM_CHECK("30", pwm_ch_get(0u, &ch) == PWM_OK && ch.running == 0u, "stop not recorded");

    /* ---- [31] 命令通道 ---- */
    pwm_bsp_uninstall();
    (void)pwm_ch_init();
    PWM_CHECK("31", pwm_cmd("init") == PWM_OK, "cmd init");
    PWM_CHECK("31", pwm_cmd("ch 1 50 1000") == PWM_OK, "cmd ch");
    PWM_CHECK("31", pwm_ch_get(1u, &ch) == PWM_OK && ch.hz == 1000u && ch.duty_pct == 50u,
              "cmd ch did not take effect");
    PWM_CHECK("31", pwm_cmd("reg 3") == (int)HW_PWM_EMC, "cmd reg");
    PWM_CHECK("31", pwm_cmd("reg 7") == PWM_ERR_BOUND, "cmd reg oob");
    PWM_CHECK("31", pwm_cmd("regcnt") == (int)PWM_REG_MAX, "cmd regcnt");
    PWM_CHECK("31", pwm_cmd("bsp") == 0, "cmd bsp (SIM)");
    PWM_CHECK("31", pwm_cmd("start 1") == PWM_ERR_NOBSP, "cmd start must reject w/o BSP");
    /* 未知命令 / 空串 / NULL 都要判负, 不是静默 0 */
    PWM_CHECK("32", pwm_cmd("nosuch") == PWM_ERR_REJECT, "cmd unknown");
    PWM_CHECK("32", pwm_cmd("") == PWM_ERR_REJECT, "cmd empty");
    PWM_CHECK("32", pwm_cmd(NULL) == PWM_ERR_PARAM, "cmd NULL");
    PWM_CHECK("32", pwm_cmd("ch 1") == PWM_ERR_PARAM, "cmd ch missing args");
    PWM_CHECK("32", pwm_cmd("ch x 50 1000") == PWM_ERR_PARAM, "cmd ch non-numeric");

    /* ---- [32b] duty 查询命令 (变异 M14 实锤缺口: 该命令原本零覆盖,
     * 把 return (int)b 改成 return 0 照样全绿) ---- */
    {
        uint32_t code = 0u;
        PWM_CHECK("32b", pwm_duty_encode(50u, &code) == PWM_OK, "duty encode 50");
        PWM_CHECK("32b", pwm_cmd("duty 50") == (int)code,
                  "cmd duty 50 must return encoded value, not 0");
        /* 非 0 的值才具区分力: 若 code 恰为 0 则上条形同虚设, 钉住它 */
        PWM_CHECK("32b", code != 0u, "duty 50 encodes to 0 -> assertion would be toothless");
        PWM_CHECK("32b", pwm_cmd("duty 0") == 0, "cmd duty 0");
        PWM_CHECK("32b", pwm_cmd("duty 100") != pwm_cmd("duty 25"),
                  "cmd duty must distinguish 25 vs 100");
        PWM_CHECK("32b", pwm_cmd("duty 101") == PWM_ERR_PARAM, "cmd duty oob");
    }

    /* ---- [32c] get 回读命令 (变异 M16 实锤: 第一版守卫自己失效了) ----
     * 第一版写的是 pwm_cmd("get 1") == 0 且与 st.running 比对, 而无 BSP 时
     * running【恰好就是 0】⇒ 把出口换成恒 0 照样全绿 = 自算恒等的死断言。
     * 修法: 先装 BSP 真让通道 running=1, 再断言命令层读到 1。
     * 判据必须落在【与出口不同的值】上, 否则断言没有区分力。 */
    {
        pwm_ch_t st;
        PWM_CHECK("32c", pwm_cmd("get 1") == 0, "cmd get: 无 BSP 时未 start 必须 0");
        PWM_CHECK("32c", pwm_cmd("get 99") == PWM_ERR_BOUND, "cmd get oob channel");

        /* 装【桩 BSP】→ start 成功 → running 真的变 1
         * 🕳️ 头一版这里写的是 pwm_bsp_install(sv_rd, sv_wr, sv_usr), 4 条全红。
         * 根因不是被测物坏: sv_* 是【自检入口时的 BSP 快照】, 只该在【结尾还原】用
         * (见文件末尾), 拿它当中途的工作 BSP 就是误用。宿主直接调 selftest 时入口
         * 无人装 BSP ⇒ sv_* 三者皆 NULL ⇒ 装了个空 BSP ⇒ start 诚实返回
         * ERR_NOBSP。教训: 快照变量与工作变量同名不同义, 复制粘贴最易踩。 */
        pwm_bsp_install(pwm_stub_read, pwm_stub_write, NULL);
        PWM_CHECK("32c", pwm_cmd("start 1") == PWM_OK, "cmd start with BSP must succeed");
        PWM_CHECK("32c", pwm_ch_get(1u, &st) == PWM_OK && st.running == 1u,
                  "api running must really be 1 (else next assert is toothless)");
        /* ★核心判据: 期望值是 1, 出口若被换成恒 0 必被抓 ★ */
        PWM_CHECK("32c", pwm_cmd("get 1") == 1, "cmd get must report running=1 (constant-0 mutant!)");
        PWM_CHECK("32c", pwm_cmd("get 0") == 0, "cmd get ch0 (never started) must be 0");
        PWM_CHECK("32c", pwm_cmd("get 1") != pwm_cmd("get 0"),
                  "cmd get must distinguish started vs not-started channel");
        pwm_cmd("stop 1");
        PWM_CHECK("32c", pwm_cmd("get 1") == 0, "cmd get after stop must be 0");
        pwm_bsp_uninstall();
    }

    /* ---- [33] 模式名表自洽 (六模式) ---- */
    for (i = 0u; i < (uint32_t)HW_PWM_MODE_MAX; i++) {
        PWM_CHECK("33", strcmp(pwm_mode_str((uint8_t)i), "??") != 0, "mode name missing");
    }
    PWM_CHECK("33", (uint32_t)pwm_mode() < (uint32_t)HW_PWM_MODE_MAX, "mode out of range");
    PWM_CHECK("33", strcmp(pwm_mode_str(HW_PWM_MODE_MAX), "??") == 0, "mode oob");
    /* 🕳️ 变异 M15 实锤存活: 上面只判 != "??" , 把 "HOST" 改成 "HOSTX" 照样全绿。
     * 必须逐个比对【具体名字】, 且要求两两互不相同(防复制粘贴式塌缩)。 */
    {
        static const char* const k_want[HW_PWM_MODE_MAX] = {
            "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
        };
        for (i = 0u; i < (uint32_t)HW_PWM_MODE_MAX; i++) {
            PWM_CHECK("33", strcmp(pwm_mode_str((uint8_t)i), k_want[i]) == 0,
                      "mode name must match expected exactly");
        }
        for (i = 0u; i < (uint32_t)HW_PWM_MODE_MAX; i++) {
            uint32_t j;
            for (j = i + 1u; j < (uint32_t)HW_PWM_MODE_MAX; j++) {
                PWM_CHECK("33", strcmp(pwm_mode_str((uint8_t)i),
                                       pwm_mode_str((uint8_t)j)) != 0,
                          "mode names must be pairwise distinct");
            }
        }
    }

    /* ---- [34] golden 经公共出口: 命令通道与 API 必须同号 (坑 #28) ---- */
    a = 0xDEADu;
    PWM_CHECK("34", pwm_reg_addr(0u, &a) == PWM_OK, "sanity");
    PWM_CHECK("34", (uint32_t)pwm_cmd("golden") == pwm_profile_golden(),
              "cmd golden != api golden");

    /* ---- 收尾: 还原自检前的 BSP (坑 #25) ---- */
    pwm_bsp_install(sv_rd, sv_wr, sv_usr);

    if (fails == 0) {
        if (putf != NULL) {
            char m[64];
            (void)snprintf(m, sizeof(m),
                           "hw_pwm selftest: ALL PASS (%u checks)\n",
                           (unsigned)checks);
            putf(m);
        }
    } else if (putf != NULL) {
        char m[64];
        (void)snprintf(m, sizeof(m),
                       "hw_pwm selftest: %d/%u FAILED\n", fails, (unsigned)checks);
        putf(m);
    }
    return fails;
}

/* ============================================================
 * 10. CLI  (./xiaomo pwm ...)
 * ============================================================ */
static int pwm_cli_puts(const char* s) { return printf("%s", s); }

int pwm_cli(int argc, char** argv)
{
    uint32_t i, a;

    if (argc < 3) {
        printf("=== XIAOMO PWM 脉冲输出层 (hw_pwm) ===\n");
        printf("mode      = %s (%u)\n", pwm_mode_str(pwm_mode()), (unsigned)pwm_mode());
        printf("bsp       = %s\n", pwm_bsp_installed() ? "REAL (已装 BSP)" : "SIM  (表驱动)");
        printf("freq      = %u Hz (max %u)\n",
               (unsigned)HW_PWM_FREQUENCY, (unsigned)PWM_FREQ_MAX);
        printf("---- 寄存器档案 (黄金 G_REG=0x%08X) ----\n", (unsigned)PWM_GOLDEN_REG);
        for (i = 0u; i < (uint32_t)PWM_REG_MAX; i++) {
            (void)pwm_reg_addr(i, &a);
            printf("  [%u] %-5s 0x%06X\n", (unsigned)i, pwm_reg_name(i), (unsigned)a);
        }
        printf("---- 脚号档案 (黄金 G_PIN=0x%08X) ----\n", (unsigned)PWM_GOLDEN_PIN);
        for (i = 0u; i < (uint32_t)PWM_PIN_MAX; i++) {
            (void)pwm_pin_addr(i, &a);
            printf("  [%u] %-5s %u\n", (unsigned)i, pwm_pin_name(i), (unsigned)a);
        }
        printf("---- 通道 ----\n");
        for (i = 0u; i < (uint32_t)PWM_CH_MAX; i++) {
            pwm_ch_t c;
            if (pwm_ch_get(i, &c) == PWM_OK) {
                printf("  ch%u hz=%-7u pct=%-4u code=%-6u %s\n",
                       (unsigned)i, (unsigned)c.hz, (unsigned)c.duty_pct,
                       (unsigned)c.duty_code, c.running ? "RUN" : "stop");
            }
        }
        printf("golden    = 0x%08X (ALL) / 0x%08X (REG) / 0x%08X (VEC)\n",
               (unsigned)pwm_profile_golden(), (unsigned)pwm_golden_reg(),
               (unsigned)pwm_golden_vec());
        printf("---- 自检 ----\n");
        return pwm_selftest(pwm_cli_puts);   /* 失败计数直通退出码 (坑 #50) */
    }

    /* 🔴 selftest 必须直通返回码, 不能塞进 return 0 (坑 #50) */
    if (strcmp(argv[2], "selftest") == 0) {
        return pwm_selftest(pwm_cli_puts);
    }
    return pwm_cmd(argv[2]);
}

#ifdef __cplusplus
}
#endif
