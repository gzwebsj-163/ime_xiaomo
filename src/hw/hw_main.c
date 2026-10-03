/*
 * xiaomo - hw 家族统一类注册表 + 总调度 (hw_main, 2026-09-28)
 *
 * 全跨式设计 (与 hw_token/hw_fault/hw_core 同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      -DHW_MAIN_MODE_OVERRIDE=n 可强指任意模式做编译验证
 *      (KELL 内核内 __linux__ 亦须 -DHW_MAIN_KELL=1 抢先识别)。
 *   2. 仅依赖 stdint+string+stdio, 无 malloc/无 pthread/无文件 IO,
 *      真机可 freestanding 编译 (探针不碰磁盘: hw_asr 只读剧目录)。
 *   3. 家族成员经「弱引用」接入: 真机固件可按需裁剪任意 hw_* 模块,
 *      未链接成员探针返回 HW_MAIN_RC_NOLINK, 其余字段仍有效
 *      (裁剪不破坏类表结构/黄金校验和)。
 *   4. 黄金参考: HW_MAIN_GOLDEN = 类表 (name + cls) FNV-1a-32
 *      = 0x5FAD755A (独立 Python 对拍锁定 2026-09-28, 勿改)。
 *
 * 六层接入 (2026-09-28):
 *   Makefile   HW_SRCS 收编 (macOS + Makefile.linux)
 *   VM 内核    OP_HW_MAIN_CALL (vm_core.c, kvm_run 上电自动 hw_main_init)
 *   编译器     mo2kbc 内置 hw_main("...") / hw_main("...", 数值)
 *   CLI        ./xiaomo main [card|selftest|count|idx N|find ID|mode|sum|ok|probe ID|probeall]
 *   示例       examples/hwmain_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh hwmain 块
 *
 * ⚠️ C++ 编译纪律: 弱引用声明必须包 extern "C" (mangled 名对不上 →
 *    弱符号永远查不到 → 假 NOLINK)。本文件不 include 任何兄弟模块头
 *    (hw_dev.h 含 C++ using 语法), 弱声明签名以注释锚定兄弟头文件。
 */
#include "hw_main.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 黄金校验和: 类表 (name+cls) FNV-1a-32, Python 对拍锁定。
 * 2026-10-03 登记 USBPD 后由 0x5FAD755A 变更为 0xA57E74DF。
 * 🕳️ 换算法/改类表必须重算, 且必须用【独立 Python】复算 —— 先让 Python
 *    复现旧值 0x5FAD755A 自证口径正确, 再让它算新值。 */
#define HW_MAIN_GOLDEN 0xA57E74DFu

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t main_mode_probe(void)
{
#if defined(HW_MAIN_MODE_OVERRIDE)
    return (uint8_t)HW_MAIN_MODE_OVERRIDE;
#elif defined(HW_MAIN_KELL)
    return (uint8_t)HW_MAIN_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_MAIN_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_MAIN_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_MAIN_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_MAIN_MODE_HOST;
#else
    return (uint8_t)HW_MAIN_MODE_TEST;
#endif
}

uint8_t hw_main_mode(void)
{
    static uint8_t cached = 0xFFu;   /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = main_mode_probe();
    return cached;
}

static const char* const main_mode_names[HW_MAIN_MODE_TEST + 1] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_main_mode_str(uint8_t mode)
{
    return (mode <= HW_MAIN_MODE_TEST) ? main_mode_names[mode] : "?";
}

/* ============================================================
 * 2. 家族成员弱引用 (真机固件可裁剪, 未链接 → 探针 NOLINK)
 *    签名锚定 (改动兄弟头文件须同步):
 *      hw_token.h  int hw_token_selftest(int (*)(const char*))
 *      hw_core.h   int hw_core_selftest(int (*)(const char*))
 *      hw_fault.h  int hw_fault_selftest(int (*)(const char*))
 *      hw_asr.h    int hw_asr_list(char [][64], int[], int)
 *      hw_dev.h    uint32_t hw_dev_registered(void)
 *      hw_direct.h const char* hw_get_error(void)
 *      hw_oem.h    uint64_t hw_oem_sig(void)
 *      hw_hex.h    char* hw_hex_fmt64(uint64_t, char*, int) /
 *                  uint64_t hw_hex_parse64(const char*)
 * ============================================================ */
#ifdef __cplusplus
extern "C" {
#endif
extern int          hw_token_selftest(int (*)(const char*)) __attribute__((weak));
extern int          hw_core_selftest(int (*)(const char*)) __attribute__((weak));
extern int          hw_fault_selftest(int (*)(const char*)) __attribute__((weak));
extern int          hw_asr_list(char (*)[64], int*, int) __attribute__((weak));
extern unsigned int hw_dev_registered(void) __attribute__((weak));
extern const char*  hw_get_error(void) __attribute__((weak));
extern unsigned long long hw_oem_sig(void) __attribute__((weak));
extern char*        hw_hex_fmt64(unsigned long long, char*, int) __attribute__((weak));
extern unsigned long long hw_hex_parse64(const char*) __attribute__((weak));
/* hw_usbpd 唯一真表出口。签名锚定 (include/hw_usbpd.h)。
 * 🕳️ 这里刻意【不】#include "hw_usbpd.h": 与家族其余探针同款只声明用到的弱符号,
 *    真机固件裁剪掉 usbpd/ 时头可能不存在, 且本文件只需指针非空 + 行数。
 *    本探针【不解引用】返回的表指针, 因此不依赖结构体布局是否一致。 */
extern const void*  hw_usbpd_qc_table(int* n_rows) __attribute__((weak));
#ifdef __cplusplus
}
#endif

/* ---- 各类健康探针 (无副作用; 未链接 → HW_MAIN_RC_NOLINK) ---- */
static int main_probe_asr(void)
{
    if (hw_asr_list == 0) return HW_MAIN_RC_NOLINK;   /* 只读剧目录, max=0 不写 */
    return (hw_asr_list(NULL, NULL, 0) >= 0) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_core(void)
{
    if (hw_core_selftest == 0) return HW_MAIN_RC_NOLINK;
    return (hw_core_selftest(NULL) == 0) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_dev(void)
{
    if (hw_dev_registered == 0) return HW_MAIN_RC_NOLINK;
    return HW_MAIN_RC_OK;   /* 注册数 >= 0 恒真: API 表可即达 */
}
static int main_probe_direct(void)
{
    if (hw_get_error == 0) return HW_MAIN_RC_NOLINK;
    return (hw_get_error() != NULL) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_fault(void)
{
    if (hw_fault_selftest == 0) return HW_MAIN_RC_NOLINK;
    return (hw_fault_selftest(NULL) == 0) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_hex(void)
{
    char buf[32];
    if (hw_hex_fmt64 == 0 || hw_hex_parse64 == 0) return HW_MAIN_RC_NOLINK;
    if (hw_hex_fmt64(0x123456789ABCDEF0ULL, buf, (int)sizeof(buf)) == NULL)
        return HW_MAIN_RC_BAD;
    return (hw_hex_parse64(buf) == 0x123456789ABCDEF0ULL)
         ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_oem(void)
{
    if (hw_oem_sig == 0) return HW_MAIN_RC_NOLINK;
    return (hw_oem_sig() == 0x5849414F4D4F3031ULL) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
static int main_probe_token(void)
{
    if (hw_token_selftest == 0) return HW_MAIN_RC_NOLINK;
    return (hw_token_selftest(NULL) == 0) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}
/* 🕳️ 探针必须有区分力 (锚点 H: 恒真信号比假信号更险, 因为它让你"有依据地"放心)。
 *   这里取的是 selftest [1] 的同款判据 (真表非塌: 指针非 NULL 且 n >= 4),
 *   所以判据表被掏成空/塌行时 `xiaomo main probeall` 会真的报 BAD, 而不是恒绿。 */
static int main_probe_usbpd(void)
{
    int n = 0;
    const void* t;
    if (hw_usbpd_qc_table == 0) return HW_MAIN_RC_NOLINK;
    t = hw_usbpd_qc_table(&n);
    return (t != 0 && n >= 4) ? HW_MAIN_RC_OK : HW_MAIN_RC_BAD;
}

/* ============================================================
 * 3. 类注册表 (黄金校验和保护)
 * ============================================================ */
typedef struct {
    const char* name;           /* 类名 (大写) */
    int         cls;           /* 类 ID (用户定版) */
    int       (*probe)(void);  /* 健康探针 */
} hw_main_ent_t;

static const hw_main_ent_t g_classes[HW_MAIN_CLASS_MAX] = {
    { "ASR",    (int)ASR_CLASS,    main_probe_asr    },
    { "CORE",   (int)CORE_CLASS,   main_probe_core   },
    { "DEV",    (int)DEV_CLASS,    main_probe_dev    },
    { "DIRECT", (int)DIRECT_CLASS, main_probe_direct },
    { "FAULT",  (int)FAULT_CLASS,  main_probe_fault  },
    { "HEX",    (int)HEX_CLASS,    main_probe_hex    },
    { "OEM",    (int)OEM_CLASS,    main_probe_oem    },
    { "TOKEN",  (int)TOKEN_CLASS,  main_probe_token  },
    { "USBPD",  (int)USBPD_CLASS,  main_probe_usbpd  }
};

/* 各类静态信息 (xxx_class() 返回 const int[4] = {cls, idx, mode, probe}) */
static int g_info[HW_MAIN_CLASS_MAX][4];

/* 探针结果缓存: 初始 UNPROBED, hw_main_init 复位 */
static int g_probe_cache[HW_MAIN_CLASS_MAX];

uint8_t hw_main_class_count(void) { return (uint8_t)HW_MAIN_CLASS_MAX; }

const char* hw_main_class_name(uint8_t idx)
{
    return (idx < HW_MAIN_CLASS_MAX) ? g_classes[idx].name : "?";
}

int hw_main_class_id(uint8_t idx)
{
    return (idx < HW_MAIN_CLASS_MAX) ? g_classes[idx].cls : -1;
}

int hw_main_find(int cls)
{
    int i;
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++)
        if (g_classes[i].cls == cls) return i;
    return -1;
}

/* FNV-1a-32 黄金校验和 (与 tmp/hw_main_golden.py 逐字节一致):
 * 序列 = [count] + 每类 (name 各字节 + ' ' + cls 低4字节小端 + '\n') */
uint32_t hw_main_checksum(void)
{
    uint32_t h = 2166136261u;
    int i, b;
    h ^= (uint32_t)HW_MAIN_CLASS_MAX;
    h *= 16777619u;
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
        const char* p = g_classes[i].name;
        uint32_t cls = (uint32_t)g_classes[i].cls;
        while (*p) { h ^= (uint32_t)(uint8_t)(*p++); h *= 16777619u; }
        h ^= 0x20u; h *= 16777619u;
        for (b = 0; b < 4; b++) { h ^= (cls >> (8 * b)) & 0xFFu; h *= 16777619u; }
        h ^= 0x0Au; h *= 16777619u;
    }
    return h;
}

/* ============================================================
 * 4. 健康探针 (单类现场探 / 全家族扫) + 信息数组
 * ============================================================ */
int hw_main_probe_one(uint8_t idx)
{
    int rc;
    if (idx >= HW_MAIN_CLASS_MAX) return HW_MAIN_RC_BAD;
    rc = g_classes[idx].probe();
    g_probe_cache[idx] = rc;
    g_info[idx][3] = rc;
    return rc;
}

static int main_puts_or(int (*putf)(const char*), const char* s)
{
    if (putf) return putf(s);
    return 0;
}

int hw_main_probe_all(int (*putf)(const char*))
{
    int i, bad = 0;
    char line[96];
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
        int rc = hw_main_probe_one((uint8_t)i);
        const char* tag = (rc == HW_MAIN_RC_OK) ? "OK"
                        : (rc == HW_MAIN_RC_NOLINK) ? "nolink" : "BAD";
        snprintf(line, sizeof(line), "  [%d] %-6s cls=0x%04X  probe=%s",
                 i, g_classes[i].name, (unsigned)g_classes[i].cls, tag);
        main_puts_or(putf, line);
        if (rc == HW_MAIN_RC_BAD) bad++;
    }
    return bad;
}

static const int* main_info_fill(uint8_t idx, int live)
{
    static const int dummy[4] = { 0, 0, 0, 0 };
    if (idx >= HW_MAIN_CLASS_MAX) return dummy;
    g_info[idx][0] = g_classes[idx].cls;
    g_info[idx][1] = (int)idx;
    g_info[idx][2] = (int)hw_main_mode();
    if (live || g_probe_cache[idx] == HW_MAIN_RC_UNPROBED)
        (void)hw_main_probe_one(idx);
    else
        g_info[idx][3] = g_probe_cache[idx];
    return g_info[idx];
}

/* ---- 用户草稿函数定版 (args==NULL 只读; args[0]==1 现场重探) ---- */
const int* asr_class(int* args)
{
    return main_info_fill(HW_CLASS_ASR, args != NULL && args[0] == 1);
}
const int* core_class(int* handler)
{
    return main_info_fill(HW_CLASS_CORE, handler != NULL && handler[0] == 1);
}
const int* dev_class(int* args)
{
    return main_info_fill(HW_CLASS_DEV, args != NULL && args[0] == 1);
}
const int* direct_class(int* args)
{
    return main_info_fill(HW_CLASS_DIRECT, args != NULL && args[0] == 1);
}
const int* fault_class(int* args)
{
    return main_info_fill(HW_CLASS_FAULT, args != NULL && args[0] == 1);
}
const int* hex_class(int* args)
{
    return main_info_fill(HW_CLASS_HEX, args != NULL && args[0] == 1);
}
const int* oem_class(int* args)
{
    return main_info_fill(HW_CLASS_OEM, args != NULL && args[0] == 1);
}
const int* token_class(int* args)
{
    return main_info_fill(HW_CLASS_TOKEN, args != NULL && args[0] == 1);
}

const int* usbpd_class(int* args)
{
    return main_info_fill(HW_CLASS_USBPD, args != NULL && args[0] == 1);
}

/* ============================================================
 * 5. 上电初始化 + 字符串命令分发 (VM/CLI 同路)
 * ============================================================ */
static int g_main_inited = 0;

void hw_main_init(void* arg)
{
    int i;
    (void)arg;
    if (g_main_inited) return;
    g_main_inited = 1;
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
        g_probe_cache[i] = HW_MAIN_RC_UNPROBED;
        g_info[i][0] = g_classes[i].cls;
        g_info[i][1] = i;
        g_info[i][2] = (int)hw_main_mode();
        g_info[i][3] = HW_MAIN_RC_UNPROBED;
    }
}

void hw_main_hook(void* arg) { (void)arg; }

/* 命令表: count/sum/ok/mode/slots 语义对齐家族; idx N/find ID/probe ID 双参可拼接 */
int hw_main_cmd(const char* cmd, void* arg)
{
    char op[16];
    long val;
    (void)arg;
    if (!cmd) return -1;
    if (sscanf(cmd, "%15s %ld", op, &val) < 1) return -1;
    if (strcmp(op, "count") == 0) return (int)HW_MAIN_CLASS_MAX;
    if (strcmp(op, "sum") == 0)
        /* 🕳️ 此处曾有 (int)(HW_MAIN_GOLDEN & 0x7FFFFFFFu) —— 摘掉掩码的同款病:
         *   旧黄金 0x5FAD755A < 2^31 时掩码是静默空操作, 登记 USBPD 后黄金
         *   0xA57E74DF 跨过 2^31, 掩码立刻生效, cmd 出口与 selftest/CLI 内部
         *   两个出口对同一份数据给出不同值 (实测 .mo 侧 0x257E74DF vs 真值
         *   0xA57E74DF)。与 hw_dmc.c:1150 记录的"曾踩"完全同型, 修法同款:
         *   cmd 出口直接给宏本身, 再加一条双出口一致性护栏把它钉死。
         *   注: 黄金 ≥ 2^31 时 (int) 是负数, 这是如实回传, 不是新 bug。 */
        return (hw_main_checksum() == HW_MAIN_GOLDEN)
             ? (int)(HW_MAIN_GOLDEN) : -1;
    if (strcmp(op, "ok") == 0)
        return (hw_main_checksum() == HW_MAIN_GOLDEN) ? 1 : 0;
    if (strcmp(op, "mode") == 0) return (int)hw_main_mode();
    if (strcmp(op, "idx") == 0) {
        if (val < 0 || val >= (long)HW_MAIN_CLASS_MAX) return -1;
        return hw_main_class_id((uint8_t)val);
    }
    if (strcmp(op, "find") == 0) {
        int i = hw_main_find((int)val);
        return i;
    }
    if (strcmp(op, "probe") == 0) {
        int i = hw_main_find((int)val);
        if (i < 0) return -1;
        return hw_main_probe_one((uint8_t)i);
    }
    if (strcmp(op, "probeall") == 0)
        return hw_main_probe_all(NULL);
    if (strcmp(op, "selftest") == 0)
        return hw_main_selftest(NULL);
    return -1;
}

/* ============================================================
 * 6. 自检 (黄金锁定/表序对拍/探针缓存生命周期/命令分发)
 *    任何平台失败数必须为 0; putf=NULL 静默 (真机固件)
 * ============================================================ */
int hw_main_selftest(int (*putf)(const char*))
{
    int fails = 0;
    char line[96];
    int i;

    /* [1] 类计数 */
    if (hw_main_class_count() != HW_MAIN_CLASS_MAX) fails++;
    snprintf(line, sizeof(line), "hwmain: [1] count = %d %s",
             (int)hw_main_class_count(),
             (hw_main_class_count() == HW_MAIN_CLASS_MAX) ? "OK" : "FAIL");
    main_puts_or(putf, line);

    /* [2] 黄金校验和 */
    {
        uint32_t c = hw_main_checksum();
        if (c != HW_MAIN_GOLDEN) fails++;
        snprintf(line, sizeof(line), "hwmain: [2] golden = 0x%08X %s",
                 (unsigned)c, (c == HW_MAIN_GOLDEN) ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [3] 类 ID ↔ 表序 双向对拍 (用户定版 8 ID 全在表) */
    {
        static const int ids[HW_MAIN_CLASS_MAX] = {
            (int)ASR_CLASS, (int)CORE_CLASS, (int)DEV_CLASS, (int)DIRECT_CLASS,
            (int)FAULT_CLASS, (int)HEX_CLASS, (int)OEM_CLASS, (int)TOKEN_CLASS,
            (int)USBPD_CLASS
        };
        int ok3 = 1;
        for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
            if (hw_main_class_id((uint8_t)i) != ids[i]) ok3 = 0;
            if (hw_main_find(ids[i]) != i) ok3 = 0;
        }
        if (hw_main_find(0x0777) != -1) ok3 = 0;   /* 未挂 ID 必须 -1 */
        if (!ok3) fails++;
        snprintf(line, sizeof(line), "hwmain: [3] cls<->idx round-trip %s",
                 ok3 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [4] 类名表序 */
    {
        static const char* const names[HW_MAIN_CLASS_MAX] = {
            "ASR", "CORE", "DEV", "DIRECT", "FAULT", "HEX", "OEM", "TOKEN",
            "USBPD"
        };
        int ok4 = 1;
        for (i = 0; i < HW_MAIN_CLASS_MAX; i++)
            if (strcmp(hw_main_class_name((uint8_t)i), names[i]) != 0) ok4 = 0;
        if (strcmp(hw_main_class_name(HW_MAIN_CLASS_MAX), "?") != 0) ok4 = 0;
        if (!ok4) fails++;
        snprintf(line, sizeof(line), "hwmain: [4] name table %s", ok4 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [5] 模式合法域 */
    {
        int ok5 = (hw_main_mode() <= HW_MAIN_MODE_TEST) ? 1 : 0;
        if (!ok5) fails++;
        snprintf(line, sizeof(line), "hwmain: [5] mode = %d (%s) %s",
                 (int)hw_main_mode(), hw_main_mode_str(hw_main_mode()),
                 ok5 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [6] 探针缓存生命周期: 现场 probe → 缓存读一致 */
    {
        const int* inf;
        int live, cached;
        int ok6 = 1;
        live = hw_main_probe_one(HW_CLASS_HEX);
        inf = hex_class(NULL);              /* args==NULL → 读缓存 */
        cached = inf[3];
        if (live != cached) ok6 = 0;
        if (inf[0] != (int)HEX_CLASS || inf[1] != (int)HW_CLASS_HEX) ok6 = 0;
        if (live == HW_MAIN_RC_BAD) ok6 = 0;  /* 宿主全链接: hex 必健康 */
        if (!ok6) fails++;
        snprintf(line, sizeof(line), "hwmain: [6] probe cache (hex live=%d cached=%d) %s",
                 live, cached, ok6 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [7] 家族探针全扫 (BAD 计失败; NOLINK=合法裁剪不计) */
    {
        int bad = hw_main_probe_all(NULL);
        int ok7 = 1;
        (void)bad;   /* 逐行明细已在缓存, 下方按缓存判定 */
        for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
            if (g_probe_cache[i] == HW_MAIN_RC_BAD) ok7 = 0;
        }
        if (!ok7) fails++;
        snprintf(line, sizeof(line), "hwmain: [7] probe all linked healthy %s",
                 ok7 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    /* [8] 命令分发对拍 (单参/全家族) */
    {
        int ok8 = 1;
        if (hw_main_cmd("count", 0) != (int)HW_MAIN_CLASS_MAX) ok8 = 0;
        if (hw_main_cmd("ok", 0) != 1) ok8 = 0;
        if (hw_main_cmd("idx 3", 0) != (int)DIRECT_CLASS) ok8 = 0;
        if (hw_main_cmd("find 152", 0) != (int)HW_CLASS_CORE) ok8 = 0;  /* 152 = 0x98 */
        if (hw_main_cmd("probe 152", 0) != HW_MAIN_RC_OK) ok8 = 0;     /* 宿主 core 健康 */
        if (hw_main_cmd("bogus", 0) != -1) ok8 = 0;
        /* 🕳️ 双出口一致性 (掩码坑的回归护栏, 修法同 hw_dmc.c [19]):
         *   cmd 出口必须与 HW_MAIN_GOLDEN 宏【逐位相等】, 不得再掩 0x7FFFFFFF。
         *   这条断言在旧黄金 (0x5FAD755A < 2^31) 下恒真 = 不具区分力的信号,
         *   正是它当初没抓到这个 bug 的原因; 黄金跨 2^31 后才具备区分力。 */
        if (hw_main_cmd("sum", 0) != (int)HW_MAIN_GOLDEN) ok8 = 0;
        if (!ok8) fails++;
        snprintf(line, sizeof(line), "hwmain: [8] cmd dispatch %s", ok8 ? "OK" : "FAIL");
        main_puts_or(putf, line);
    }

    snprintf(line, sizeof(line),
             "hwmain: selftest %s (fails=%d, golden=0x%08X, mode=%s)",
             (fails == 0) ? "all PASS" : "FAIL",
             fails, (unsigned)hw_main_checksum(),
             hw_main_mode_str(hw_main_mode()));
    main_puts_or(putf, line);
    return fails;
}

/* ============================================================
 * 7. CLI: ./xiaomo main [...]
 * ============================================================ */
static int main_cli_puts(const char* s) { puts(s); return 0; }

int hw_main_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "card";
    hw_main_init(NULL);

    if (strcmp(sub, "card") == 0 || strcmp(sub, "selftest") == 0) {
        char line[128];
        uint32_t c = hw_main_checksum();
        int i;
        puts("==== hw_main 类注册表 (hw 家族总调度) ====");
        snprintf(line, sizeof(line), "count  : %d", (int)hw_main_class_count());
        puts(line);
        snprintf(line, sizeof(line), "mode   : %s (%d)",
                 hw_main_mode_str(hw_main_mode()), (int)hw_main_mode());
        puts(line);
        snprintf(line, sizeof(line), "golden : 0x%08X %s", (unsigned)c,
                 (c == HW_MAIN_GOLDEN) ? "OK" : "FAIL");
        puts(line);
        for (i = 0; i < (int)HW_MAIN_CLASS_MAX; i++) {
            int rc = hw_main_probe_one((uint8_t)i);
            const char* tag = (rc == HW_MAIN_RC_OK) ? "OK"
                            : (rc == HW_MAIN_RC_NOLINK) ? "nolink" : "BAD";
            snprintf(line, sizeof(line), "  [%d] %-6s cls=0x%04X  probe=%s",
                     i, hw_main_class_name((uint8_t)i),
                     (unsigned)hw_main_class_id((uint8_t)i), tag);
            puts(line);
        }
        if (strcmp(sub, "selftest") == 0)
            return (hw_main_selftest(main_cli_puts) == 0) ? 0 : 1;
        puts("selftest: ./xiaomo main selftest");
        return (c == HW_MAIN_GOLDEN) ? 0 : 1;
    }
    if (strcmp(sub, "count") == 0) { printf("%d\n", (int)hw_main_class_count()); return 0; }
    if (strcmp(sub, "mode") == 0) {
        printf("%s (%d)\n", hw_main_mode_str(hw_main_mode()), (int)hw_main_mode());
        return 0;
    }
    if (strcmp(sub, "sum") == 0 || strcmp(sub, "ok") == 0) {
        uint32_t c = hw_main_checksum();
        if (strcmp(sub, "sum") == 0) { printf("0x%08X\n", (unsigned)c); }
        else { printf("%d\n", (c == HW_MAIN_GOLDEN) ? 1 : 0); }
        return (c == HW_MAIN_GOLDEN) ? 0 : 1;
    }
    if (strcmp(sub, "idx") == 0 && argc >= 4) {
        printf("0x%04X\n", (unsigned)hw_main_class_id((uint8_t)strtol(argv[3], NULL, 0)));
        return 0;
    }
    if (strcmp(sub, "find") == 0 && argc >= 4) {
        printf("%d\n", hw_main_find((int)strtol(argv[3], NULL, 0)));
        return 0;
    }
    if (strcmp(sub, "probe") == 0 && argc >= 4) {
        int i = hw_main_find((int)strtol(argv[3], NULL, 0));
        if (i < 0) { printf("-1 (未挂类 ID)\n"); return 1; }
        printf("%d (%s)\n", hw_main_probe_one((uint8_t)i),
               hw_main_class_name((uint8_t)i));
        return 0;
    }
    if (strcmp(sub, "probeall") == 0) {
        int bad = hw_main_probe_all(main_cli_puts);
        printf(bad ? "probeall: %d BAD\n" : "probeall: all healthy\n", bad);
        return (bad == 0) ? 0 : 1;
    }
    puts("usage: xiaomo main [card|selftest|count|idx N|find ID|mode|sum|ok|probe ID|probeall]");
    return 1;
}
