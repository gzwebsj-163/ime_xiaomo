/*
 * xiaomo - 跨模式域令牌层 (hw_token)
 *
 * 设计要点 (对应 hw_token.h 注释):
 *   1. 派生 hw_token_derive() 只用无符号 64 位整数的异或/乘/移位 —— 无符号
 *      溢出按模 2^64 回绕是 C 标准定义行为, 与宿主字节序/符号性/
 *      编译器无关, 六种模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST)
 *      逐位一致。
 *   2. HW_TOKEN_GOLDEN[] 是 2026-09-13 由独立 C 实现与 Python 实现
 *      对拍锁定的黄金参考值; 运行期派生必须逐位等于此表
 *      (selftest 逐域断言), 防止未来误改派生公式破坏跨历史一致性。
 *   3. 线格式 "TK1.<t>.<m>.<seq>.<value16hex>.<ck8hex>",
 *      ck = FNV-1a-32 覆盖含尾点的前缀 —— 任何字段被篡改都会校验失败。
 *   4. 仅依赖 stdint.h + (宿主 CLI)mowlib 的 stdio/string;
 *      派生/签发/验证/seal/open 本体 freestanding 可编译。
 *   5. seq 计数器为 volatile uint32_t 单调递增, 不加锁
 *      (单核 VM 场景足够; 多核并发签发序号可能乱序但值仍正确)。
 */
#include "hw_token.h"
#include <stdio.h>
#include <string.h>

/* ============================================================
 * 1. 黄金参考表 (独立实现对拍锁定, 2026-09-13)
 * ============================================================ */

static const uint64_t HW_TOKEN_GOLDEN[HW_TOKEN_TYPE_MAX] = {
    /* [0x00] KEY      */ 0xA17304E98EE05110ULL,
    /* [0x01] OEM      */ 0xBEC0DB39AAF5DF8CULL,
    /* [0x02] HWD      */ 0x48FDE1EE573298D1ULL,
    /* [0x03] POINT    */ 0x9CE60174338CF045ULL,
    /* [0x04] SELF     */ 0xFFAF3B5211309E66ULL,
    /* [0x05] KELL     */ 0xA2C8598088DC739BULL,
    /* [0x06] LINUX    */ 0xDC181F00989DBA83ULL,
    /* [0x07] ESP32    */ 0xD908FEC0F1D605AFULL,
    /* [0x08] ESP8266  */ 0x5D41DC9913D03431ULL,
    /* [0x09] TEST     */ 0xF288533769EA46A6ULL
};

/* 域名表 (与枚举顺序一致) */
static const char* const k_type_names[HW_TOKEN_TYPE_MAX] = {
    "KEY", "OEM", "HWD", "POINT", "SELF",
    "KELL", "LINUX", "ESP32", "ESP8266", "TEST"
};

static const char* const k_mode_names[HW_TOKEN_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

/* ============================================================
 * 2. 编译期模式探测 (全跨模式识别)
 * ============================================================ */

static uint8_t tk_mode_probe(void)
{
#if defined(HW_TOKEN_MODE_OVERRIDE)
    /* 构建系统显式强指 (优先级最高) */
    return (uint8_t)HW_TOKEN_MODE_OVERRIDE;
#elif defined(HW_TOKEN_KELL)
    /* KELL 内核内构建: TinyEMU 真内核里也是 __linux__,
     * 须由 KELL 构建系统加 -DHW_TOKEN_KELL=1 抢先识别 */
    return (uint8_t)HW_TOKEN_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_TOKEN_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_TOKEN_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_TOKEN_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_TOKEN_MODE_HOST;
#else
    return (uint8_t)HW_TOKEN_MODE_TEST;
#endif
}

uint8_t hw_token_mode(void)
{
    static uint8_t cached = 0xFFu;   /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = tk_mode_probe();
    return cached;
}

/* ============================================================
 * 3. 派生核心 (纯无符号整型, 全模式逐位一致)
 * ============================================================ */

/* round 常量: 黄金圆比率 (Golden ratio fractional), 与域盐/掩码无关 */
#define TK_GOLDEN_RATIO  0x9E3779B97F4A7C15ULL

/* murmur3 fmix64 风格终混 (公开域的标准混淆轮) */
static uint64_t tk_mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 29;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 32;
    return h;
}

uint64_t hw_token_derive(uint8_t type)
{
    if ((uint32_t)type >= (uint32_t)HW_TOKEN_TYPE_MAX) return 0;
    uint64_t h = HW_TOKEN_DOMAIN ^ ((uint64_t)type * TK_GOLDEN_RATIO);
    h = tk_mix(h);
    return h ^ HW_TOKEN_MASK;
}

/* ============================================================
 * 4. 字符串形式
 * ============================================================ */

const char* hw_token_type_str(uint8_t type)
{
    if ((uint32_t)type >= (uint32_t)HW_TOKEN_TYPE_MAX) return "??";
    return k_type_names[type];
}

const char* hw_token_mode_str(uint8_t mode)
{
    if ((uint32_t)mode >= (uint32_t)HW_TOKEN_MODE_MAX) return "??";
    return k_mode_names[mode];
}

const char* hw_token_flags_str(uint16_t flags)
{
    switch (flags & (HW_TOKEN_F_SEALED | HW_TOKEN_F_STICKY)) {
    case HW_TOKEN_F_SEALED | HW_TOKEN_F_STICKY: return "SEALED|STICKY";
    case HW_TOKEN_F_SEALED:                     return "SEALED";
    case HW_TOKEN_F_STICKY:                     return "STICKY";
    case HW_TOKEN_F_NONE:                       return "NATIVE";
    default:                                    return "??";
    }
}

/* ============================================================
 * 5. 签发 / 验证 (seq 计数器)
 * ============================================================ */

static volatile uint32_t g_issued[HW_TOKEN_TYPE_MAX];

uint32_t hw_token_seq(uint8_t type)
{
    if ((uint32_t)type >= (uint32_t)HW_TOKEN_TYPE_MAX) return 0;
    return g_issued[type];
}

int hw_token_issue(uint8_t type, hw_token_t* out)
{
    if (!out || (uint32_t)type >= (uint32_t)HW_TOKEN_TYPE_MAX) return -1;
    out->type  = type;
    out->mode  = hw_token_mode();
    out->flags = HW_TOKEN_F_NONE;
    if (type != HW_KEY_TOKEN) {
        out->seq = ++g_issued[type];
    } else {
        out->seq = 1;   /* KEY = 根令牌, STICKY 域不递增, 永远第 1 张 */
    }
    out->value = hw_token_derive(type);
    return 0;
}

int hw_token_verify(const hw_token_t* tk)
{
    if (!tk) return -1;
    if ((uint32_t)tk->type >= (uint32_t)HW_TOKEN_TYPE_MAX) return -1;
    if ((uint32_t)tk->mode >= (uint32_t)HW_TOKEN_MODE_MAX) return -1;
    if (tk->value != hw_token_derive(tk->type)) return -1;
    return 0;
}

/* ============================================================
 * 6. 跨模式封印/解封 (文本线格式 + FNV-1a-32 校验和)
 * ============================================================ */

#define TK_CK_OFFSET 2166136261u
#define TK_CK_PRIME  16777619u

static uint32_t tk_ck32(const char* s, uint32_t n)
{
    uint32_t h = TK_CK_OFFSET;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= TK_CK_PRIME;
    }
    return h;
}

static int tk_hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 严格解析无符号十进制 (0..limit), 非法返回 -1 */
static int tk_parse_dec(const char* s, uint32_t len, uint32_t limit, uint32_t* out)
{
    uint32_t v = 0;
    uint32_t i;
    if (len == 0 || len > 10) return -1;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > limit) return -1;
    }
    *out = v;
    return 0;
}

/* 严格解析 16 位大写/小写 hex -> uint64 */
static int tk_parse_hex64(const char* s, uint32_t len, uint64_t* out)
{
    uint64_t v = 0;
    uint32_t i;
    if (len == 0 || len > 16) return -1;
    for (i = 0; i < len; i++) {
        int d = tk_hexv((unsigned char)s[i]);
        if (d < 0) return -1;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 0;
}

static uint32_t tk_hex64(char* dst, uint64_t v)
{
    static const char* d = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++)
        dst[i] = d[(v >> ((15 - i) * 4)) & 0xFULL];
    dst[16] = '\0';
    return 16;
}

static uint32_t tk_hex32(char* dst, uint32_t v)
{
    static const char* d = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++)
        dst[i] = d[(v >> ((7 - i) * 4)) & 0xFu];
    dst[8] = '\0';
    return 8;
}

int hw_token_seal(uint8_t type, uint16_t flags, char* out, uint32_t cap)
{
    hw_token_t tk;
    char       hexv[24];
    char       hexc[16];
    int        n;
    uint32_t   ck;

    if (!out || cap < HW_TOKEN_STR_CAP) return -1;
    /* 原生签发一张再封印 (seal 只认 type+flags) */
    if (hw_token_issue(type, &tk) != 0) return -1;
    tk.flags = flags;

    (void)tk_hex64(hexv, tk.value);
    n = snprintf(out, cap, "TK1.%u.%u.%u.%s.",
                 (unsigned)tk.type, (unsigned)tk.mode,
                 (unsigned)tk.seq, hexv);
    if (n <= 0 || (uint32_t)n >= cap) return -1;
    ck = tk_ck32(out, (uint32_t)n);
    (void)tk_hex32(hexc, ck);
    if ((uint32_t)n + 8u + 1u > cap) return -1;
    memcpy(out + n, hexc, 9u);   /* 8 hex + NUL */
    return 0;
}

int hw_token_open(const char* s, hw_token_t* out)
{
    uint32_t len, dot[6];
    uint32_t nd = 0, i, ck_len, ck_val = 0;
    uint32_t t, m, q;
    uint64_t v;
    hw_token_t tk;

    if (!s || !out) return -1;
    len = (uint32_t)strlen(s);
    if (len < 12 || len >= HW_TOKEN_STR_CAP) return -1;
    if (s[0] != 'T' || s[1] != 'K' || s[2] != '1' || s[3] != '.') return -1;

    /* 定位 5 个 '.' (TK1. 后跟 t.m.seq.val.ck) */
    for (i = 0; i < len; i++) {
        if (s[i] == '.') {
            if (nd >= 6) return -1;
            dot[nd++] = i;
        }
    }
    if (nd != 5) return -1;

    /* 域/模式/序号 */
    if (tk_parse_dec(s + dot[0] + 1, dot[1] - dot[0] - 1,
                     HW_TOKEN_TYPE_MAX - 1u, &t) != 0) return -1;
    if (tk_parse_dec(s + dot[1] + 1, dot[2] - dot[1] - 1,
                     HW_TOKEN_MODE_MAX - 1u, &m) != 0) return -1;
    if (tk_parse_dec(s + dot[2] + 1, dot[3] - dot[2] - 1, 0xFFFFFFFFu, &q) != 0) return -1;

    /* 值 (必须 16 位 hex, 定长) */
    if (dot[4] - dot[3] - 1 != 16) return -1;
    if (tk_parse_hex64(s + dot[3] + 1, 16, &v) != 0) return -1;

    /* 校验和: 覆盖含尾点的前缀 (dot[4] 处的 '.') */
    ck_len = dot[4] + 1;
    if (len - ck_len != 8) return -1;
    {
        uint64_t ck64 = 0;
        if (tk_parse_hex64(s + ck_len, 8, &ck64) != 0) return -1;
        ck_val = (uint32_t)ck64;
    }
    if (tk_ck32(s, ck_len) != ck_val) return -1;
    tk.type  = (uint8_t)t;
    tk.mode  = (uint8_t)m;
    tk.seq   = q;
    tk.value = v;
    tk.flags = HW_TOKEN_F_SEALED;
    if (hw_token_verify(&tk) != 0) return -1;

    *out = tk;
    return 0;
}

/* ============================================================
 * 7. 自检 (19 项) 与 CLI
 * ============================================================ */

static int tk_puts_nop(const char* s) { (void)s; return 0; }

int hw_token_selftest(hw_token_puts_fn putf)
{
    int fails = 0;
    int t;
    hw_token_t tk1, tk2, opened;
    char sealed[HW_TOKEN_STR_CAP];
    uint32_t s1, s2;

    if (!putf) putf = tk_puts_nop;

    /* [1..10] 十域派生 == 黄金表 (跨模式铁律) */
    for (t = 0; t < HW_TOKEN_TYPE_MAX; t++) {
        if (hw_token_derive((uint8_t)t) != HW_TOKEN_GOLDEN[t]) {
            fails++;
            {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "[FAIL] derive(%s) != golden\n", hw_token_type_str((uint8_t)t));
                putf(msg);
            }
        }
    }
    /* [11] 非法域派生 = 0 */
    if (hw_token_derive(HW_TOKEN_TYPE_MAX) != 0) fails++;
    /* [12] 远超界域派生 = 0 */
    if (hw_token_derive(0xFFu) != 0) fails++;

    /* [13] 签发字段完整 */
    if (hw_token_issue(HW_SELF_TOKEN, &tk1) != 0 ||
        tk1.type != HW_SELF_TOKEN || tk1.mode != hw_token_mode() ||
        tk1.flags != HW_TOKEN_F_NONE || tk1.seq == 0 ||
        tk1.value != hw_token_derive(HW_SELF_TOKEN)) fails++;

    /* [14] 好令牌验证通过 */
    if (hw_token_verify(&tk1) != 0) fails++;
    /* [15] 篡改 value -> 拒 */
    tk2 = tk1; tk2.value ^= 0x1ULL;
    if (hw_token_verify(&tk2) != -1) fails++;
    /* [16] 冒名 type -> 拒 (value 跟着错) */
    tk2 = tk1; tk2.type = HW_OEM_TOKEN;
    if (hw_token_verify(&tk2) != -1) fails++;

    /* [17] seq 单调 +1 (非 STICKY 域) */
    s1 = tk1.seq;
    if (hw_token_issue(HW_SELF_TOKEN, &tk2) != 0) fails++;
    s2 = tk2.seq;
    if (s2 != s1 + 1u) fails++;

    /* [18] seal -> open 回环逐字段一致 + F_SEALED 置位 */
    if (hw_token_seal(HW_TEST_TOKEN, HW_TOKEN_F_NONE, sealed, sizeof(sealed)) != 0) fails++;
    else if (hw_token_open(sealed, &opened) != 0) fails++;
    else if (opened.type != HW_TEST_TOKEN || opened.mode != hw_token_mode() ||
             opened.value != hw_token_derive(HW_TEST_TOKEN) ||
             (opened.flags & HW_TOKEN_F_SEALED) == 0) fails++;

    /* [19] 篡改线格式 mode 字段 -> 校验和拒解 */
    if (hw_token_seal(HW_TEST_TOKEN, HW_TOKEN_F_NONE, sealed, sizeof(sealed)) == 0) {
        /* "TK1.<t>.<m>.<seq>..." 第二个 '.' 后是 mode, 单字符改写 */
        char* p = strchr(sealed, '.');
        p = p ? strchr(p + 1, '.') : 0;
        if (p && p[1] >= '0' && p[1] <= '9' && p[2] == '.') {
            p[1] = (p[1] == '9') ? '0' : (char)(p[1] + 1);   /* mode 平移一位 */
            if (hw_token_open(sealed, &opened) != -1) fails++;
        } else {
            fails++;   /* 线格式异常 (不应发生) */
        }
    } else {
        fails++;
    }

    if (fails == 0) putf("hw_token selftest: 19/19 PASS\n");
    return fails;
}

/* ---- CLI: ./xiaomo token ---- */

static int tk_cli_puts(const char* s) { return printf("%s", s); }

int hw_token_cli(void)
{
    int t;
    hw_token_t tk;
    char sealed[HW_TOKEN_STR_CAP];
    hw_token_t opened;

    printf("=== XIAOMO 跨模式域令牌 (hw_token) ===\n");
    printf("mode         = %s (%u)\n", hw_token_mode_str(hw_token_mode()),
           (unsigned)hw_token_mode());
    printf("domain salt  = 0x%016llX (\"XIAOMO01\")\n",
           (unsigned long long)HW_TOKEN_DOMAIN);
    printf("obf mask     = 0x%016llX (XOR 0x5A)\n",
           (unsigned long long)HW_TOKEN_MASK);
    printf("---- 全域令牌卡 ----\n");
    printf("%-6s %-8s %-18s %-6s %s\n", "type", "name", "value", "seq", "golden");
    for (t = 0; t < HW_TOKEN_TYPE_MAX; t++) {
        uint64_t v = hw_token_derive((uint8_t)t);
        (void)hw_token_issue((uint8_t)t, &tk);
        printf("%-6u %-8s 0x%016llX %-6u %s\n",
               (unsigned)t, hw_token_type_str((uint8_t)t),
               (unsigned long long)v, (unsigned)hw_token_seq((uint8_t)t),
               (v == HW_TOKEN_GOLDEN[t]) ? "OK" : "MISMATCH");
    }
    printf("---- seal/open 跨模式通道回环 ----\n");
    if (hw_token_seal(HW_SELF_TOKEN, HW_TOKEN_F_NONE, sealed, sizeof(sealed)) == 0 &&
        hw_token_open(sealed, &opened) == 0 &&
        opened.value == hw_token_derive(HW_SELF_TOKEN)) {
        printf("seal  = %s\n", sealed);
        printf("open  = type %s / mode %s / seq %u / value 0x%016llX / %s\n",
               hw_token_type_str(opened.type), hw_token_mode_str(opened.mode),
               (unsigned)opened.seq, (unsigned long long)opened.value,
               hw_token_flags_str(opened.flags));
        printf("loop  = OK (sealed token 逐字段还原)\n");
    } else {
        printf("loop  = FAIL\n");
    }
    printf("---- 自检 ----\n");
    {
        int fails = hw_token_selftest(tk_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
}
