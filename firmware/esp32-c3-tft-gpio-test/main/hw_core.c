/*
 * xiaomo - 内核 DNA 编码层 (hw_core, 2026-09-28)
 *
 * 全跨式设计 (与 hw_token/hw_fault 同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_CORE_MODE_OVERRIDE=n 强指 (KELL 内核内
 *      __linux__ 亦须 KELL 宏抢先识别)。
 *   2. 核心 (模式/DNA 表/槽位/校验和) 仅依赖 stdint+string,
 *      真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 纯数据编码层, 无底层硬件读取 → 无 BSP 回调 (同 hw_token);
 *      平台差异仅有模式编号, 编译期宏隔离。
 *   4. 黄金参考: HW_CORE_GOLDEN = DNA 表 (name + value) FNV-1a-32
 *      (独立 Python 对拍锁定, 2026-09-28), selftest 逐项断言防未来
 *      误改破坏跨历史一致。
 *
 * 六层接入 (2026-09-28):
 *   Makefile   HW_SRCS 收编 (macOS 已有, Makefile.linux 同补)
 *   VM 内核    OP_HW_CORE_CALL (vm_core.c, kvm_run 上电自动 hw_core_init)
 *   编译器     mo2kbc 内置 hw_core("...") → OP_HW_CORE_CALL
 *   CLI        ./xiaomo core [dna|idx N|slot N CODE|free N|slots|mode|selftest]
 *   示例       examples/hwcore_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh hwcore 块
 */
#include "hw_core.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 黄金校验和: 独立 Python 对拍锁定 (2026-09-28), 勿改 */
#define HW_CORE_GOLDEN 0xA5618C4Au

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t core_mode_probe(void)
{
#if defined(HW_CORE_MODE_OVERRIDE)
    return (uint8_t)HW_CORE_MODE_OVERRIDE;
#elif defined(HW_CORE_KELL)
    return (uint8_t)HW_CORE_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_CORE_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_CORE_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_CORE_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_CORE_MODE_HOST;
#else
    return (uint8_t)HW_CORE_MODE_TEST;
#endif
}

uint8_t hw_core_mode(void)
{
    static uint8_t cached = 0xFFu;   /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = core_mode_probe();
    return cached;
}

static const char* const core_mode_names[HW_CORE_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_core_mode_str(uint8_t mode)
{
    return (mode < HW_CORE_MODE_MAX) ? core_mode_names[mode] : "?";
}

/* ============================================================
 * 2. 内核 DNA 表 (值锁定, 黄金校验和保护)
 * ============================================================ */
typedef struct {
    const char* name;   /* DNA 名 (3 字节文本) */
    uint16_t    value;  /* 编码 = 基类 | 子码 */
} core_dna_ent_t;

static const core_dna_ent_t g_dna_table[HW_CORE_MAX] = {
    { "DEF", HW_CORE_DEF },
    { "DFF", HW_CORE_DFF },
    { "SIN", HW_CORE_SIN },
    { "SNG", HW_CORE_SNG },
    { "SID", HW_CORE_SID },
    { "DFT", HW_CORE_DFT },
    { "DDT", HW_CORE_DDT },
    { "ODT", HW_CORE_ODT },
    { "FFQ", HW_CORE_FFQ },
    { "TTU", HW_CORE_TTU }
};

/* 纯 core_dna 值数组 (2026-09-28 真机接入修复): hw_core_dna_table() 须可
 * 按 hw_core_dna_table()[i] 迭代 — 直接返回 entry.value 字段地址会因
 * entry 结构体步长 (name 指针 + value) 读花, 只允许独立数组。 */
static const core_dna g_dna_vals[HW_CORE_MAX] = {
    CORE_DNA_DEF, CORE_DNA_DFF, CORE_DNA_SIN, CORE_DNA_SNG, CORE_DNA_SID,
    CORE_DNA_DFT, CORE_DNA_DDT, CORE_DNA_ODT, CORE_DNA_FFQ, CORE_DNA_TTU
};

uint8_t hw_core_dna_count(void)
{
    return (uint8_t)HW_CORE_MAX;
}

const core_dna* hw_core_dna_table(void)
{
    return g_dna_vals;   /* 纯值数组, [i] 迭代步长正确 (entry 表勿直返) */
}

const char* hw_core_dna_name(uint8_t idx)
{
    return (idx < HW_CORE_MAX) ? g_dna_table[idx].name : "?";
}

/* 区间判定 + 名称映射见 hw_core_base_name (2026-09-28: 掩码法对
 * 子码跨高字节的 ODT 失效, 改区间判定, selftest 断言锁定) */

const char* hw_core_base_name(uint16_t code)
{
    /* 区间判定 (2026-09-28 修复): SIGNAL 基类=0x0000, 子码可跨高字节
     * (如 ODT=0x0100) — 掩码取基类对它失效, 必须按区间:
     *   0x1000..  = 编码空间外 "?" / >=0x0F00=TERM / >=0x0E00=FLOW /
     *   >=0x0D00=DATA / 其余=SIGNAL (含子码跨高字节的 ODT) */
    if (code >= 0x1000u) return "?";
    if (code >= 0x0F00u) return "TERM";
    if (code >= 0x0E00u) return "FLOW";
    if (code >= 0x0D00u) return "DATA";
    return "SIGNAL";
}

/* FNV-1a-32 over (name 字节串 + value 低字节 + value 高字节) 逐项折叠
 * — 与 Python 对拍锁定 (2026-09-28) */
uint32_t hw_core_checksum(void)
{
    uint32_t h = 0x811C9DC5u;
    uint32_t i;
    for (i = 0; i < HW_CORE_MAX; i++) {
        const unsigned char* s = (const unsigned char*)g_dna_table[i].name;
        while (*s) { h ^= (uint32_t)*s++; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
        h ^= (uint32_t)(g_dna_table[i].value & 0xFFu);
        h = (h * 0x01000193u) & 0xFFFFFFFFu;
        h ^= (uint32_t)((g_dna_table[i].value >> 8) & 0xFFu);
        h = (h * 0x01000193u) & 0xFFFFFFFFu;
    }
    return h;
}

/* ============================================================
 * 3. 槽位寄存 (用户原设计, 语义修复: core_init 返回码)
 *    槽值 0 = 空槽; DNA 编码均 > 0, 无歧义。
 * ============================================================ */
static uint16_t g_slots[HW_CORE_MAX];
static uint8_t  g_ready;   /* hw_core_init 黄金自证结果 */

void hw_core_init(void* arg)
{
    (void)arg;
    /* 上电: 黄金自证 + 槽位复位 (旧槽不跨运行残留, 同 hw_fault 注入表纪律) */
    g_ready = (hw_core_checksum() == HW_CORE_GOLDEN) ? 1u : 0u;
    memset(g_slots, 0, sizeof(g_slots));
}

void hw_core_hook(void* arg) { (void)arg; }   /* 无动态资源 */

int core_init(int handler[])
{
    uint32_t i;
    if (handler == NULL) return -1;
    for (i = 0; i < HW_CORE_MAX; i++)
        handler[i] = (int)g_dna_table[i].value;
    return 0;
}

void core_deinit(uint16_t handler[])
{
    if (handler == NULL) return;
    memset(handler, 0, sizeof(uint16_t) * HW_CORE_MAX);
}

int core_slot_load(uint8_t slot, core_dna code)
{
    if (slot >= HW_CORE_MAX) return -1;
    g_slots[slot] = (uint16_t)code;
    return 0;
}

int core_slot_free(uint8_t slot)
{
    if (slot >= HW_CORE_MAX) return -1;
    g_slots[slot] = 0u;
    return 0;
}

uint32_t core_slots_used(void)
{
    uint32_t n = 0, i;
    for (i = 0; i < HW_CORE_MAX; i++)
        if (g_slots[i] != 0u) n++;
    return n;
}

core_dna core_ject(const int point[])
{
    if (!point) return (core_dna)0;
    return (core_dna)point[0];
}

core_dna core_deject(const uint16_t point[])
{
    if (!point) return (core_dna)0;
    return (core_dna)point[0];
}

/* ============================================================
 * 4. 命令分发 (VM OP_HW_CORE_CALL / CLI 一次性)
 *    返回: >=0 结果; -1 未识别/参数非法; -2 help
 * ============================================================ */
int hw_core_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (!cmd) return -1;
    if (strcmp(cmd, "count") == 0) return (int)HW_CORE_MAX;
    if (strcmp(cmd, "ok")   == 0)
        return (hw_core_checksum() == HW_CORE_GOLDEN) ? 1 : 0;
    if (strcmp(cmd, "slots") == 0) return (int)core_slots_used();
    if (strcmp(cmd, "mode") == 0)  return (int)hw_core_mode();
    if (strcmp(cmd, "help") == 0)  return -2;
    if (strncmp(cmd, "idx ", 4) == 0) {
        long idx = strtol(cmd + 4, NULL, 10);
        if (idx < 0 || idx >= (long)HW_CORE_MAX) return -1;
        return (int)g_dna_table[idx].value;
    }
    if (strncmp(cmd, "slot ", 5) == 0) {
        long slot = 0, code = -1;
        if (sscanf(cmd + 5, "%ld %ld", &slot, &code) != 2) return -1;
        if (slot < 0 || slot >= (long)HW_CORE_MAX ||
            code < 0 || code > 0xFFFF) return -1;
        return core_slot_load((uint8_t)slot, (core_dna)code);
    }
    if (strncmp(cmd, "free ", 5) == 0) {
        long slot = strtol(cmd + 5, NULL, 10);
        if (slot < 0 || slot >= (long)HW_CORE_MAX) return -1;
        return core_slot_free((uint8_t)slot);
    }
    return -1;
}

/* ============================================================
 * 5. 自检 (全跨式黄金锁定)
 * ============================================================ */
#define CORE_CHECK(cond) do { if (!(cond)) fails++; } while (0)

int hw_core_selftest(int (*putf)(const char*))
{
    /* 期望值: 与头文件宏锁定值逐项对拍 (0x.. 十六进制) */
    static const uint16_t exp[HW_CORE_MAX] = {
        0x0DEFu, 0x0DFFu, 0x00EFu, 0x00DDu, 0x00EEu,
        0x0059u, 0x0080u, 0x0100u, 0x0EFFu, 0x0FFDu
    };
    int fails = 0;
    uint32_t i;
    char line[160];

    /* [1] 黄金校验和锁定 */
    CORE_CHECK(hw_core_checksum() == HW_CORE_GOLDEN);

    /* [2] 表序 = 宏锁定值 + 只读访问器 */
    CORE_CHECK(hw_core_dna_count() == (uint8_t)HW_CORE_MAX);
    for (i = 0; i < HW_CORE_MAX; i++) {
        CORE_CHECK(g_dna_table[i].value == exp[i]);
        CORE_CHECK(hw_core_dna_name((uint8_t)i) != NULL);
    }
    {
        const core_dna* t = hw_core_dna_table();
        CORE_CHECK(t != NULL);
        for (i = 0; i < HW_CORE_MAX; i++)
            CORE_CHECK((uint16_t)t[i] == exp[i]);   /* 纯值数组迭代 (真机坑) */
    }
    CORE_CHECK(hw_core_dna_name((uint8_t)HW_CORE_MAX) != NULL &&
               strcmp(hw_core_dna_name((uint8_t)HW_CORE_MAX), "?") == 0);

    /* [3] 基类名映射 (区间判定: ODT 子码 0x0100 跨高字节, 掩码法失效坑) */
    CORE_CHECK(strcmp(hw_core_base_name(HW_CORE_DEF), "DATA")   == 0);
    CORE_CHECK(strcmp(hw_core_base_name(HW_CORE_SIN), "SIGNAL") == 0);
    CORE_CHECK(strcmp(hw_core_base_name(HW_CORE_FFQ), "FLOW")   == 0);
    CORE_CHECK(strcmp(hw_core_base_name(HW_CORE_TTU), "TERM")  == 0);
    CORE_CHECK(strcmp(hw_core_base_name(HW_CORE_ODT), "SIGNAL") == 0);
    CORE_CHECK(strcmp(hw_core_base_name(0xABCD), "?") == 0);

    /* [4] 槽位 API: 装载/弹出/卸载/计数 + NULL 容错 */
    hw_core_init(NULL);                          /* 上电复位: 槽全空 */
    CORE_CHECK(core_slots_used() == 0);
    CORE_CHECK(core_slot_load(3, CORE_DNA_SID) == 0);   /* 0x00EE */
    CORE_CHECK(core_slots_used() == 1);
    CORE_CHECK(g_slots[3] == 0x00EEu);
    CORE_CHECK(core_slot_free(3) == 0);
    CORE_CHECK(core_slots_used() == 0);
    CORE_CHECK(core_slot_load((uint8_t)HW_CORE_MAX, CORE_DNA_DEF) == -1);
    CORE_CHECK(core_slot_free((uint8_t)HW_CORE_MAX) == -1);
    CORE_CHECK(hw_core_cmd("slot 9 4093", NULL) == 0);  /* 0x0FFD TTU */
    CORE_CHECK(hw_core_cmd("slots", NULL) == 1);
    CORE_CHECK(g_slots[9] == 0x0FFDu);
    hw_core_init(NULL);

    /* [5] 槽位句柄数组 (用户原设计): init 装载/deinit 清零/ject/deject */
    {
        int handler[HW_CORE_MAX];
        uint16_t uh[HW_CORE_MAX];
        CORE_CHECK(core_init(NULL) == -1);
        CORE_CHECK(core_init(handler) == 0);
        for (i = 0; i < HW_CORE_MAX; i++)
            CORE_CHECK((uint16_t)handler[i] == exp[i]);
        CORE_CHECK(core_ject(handler) == (core_dna)exp[0]);       /* 0x0DEF */
        CORE_CHECK(core_ject(NULL) == (core_dna)0);
        for (i = 0; i < HW_CORE_MAX; i++) uh[i] = (uint16_t)handler[i];
        CORE_CHECK(core_deject(uh) == (core_dna)exp[0]);
        core_deinit(uh);
        CORE_CHECK(core_deject(uh) == (core_dna)0);
        core_deinit(NULL);                                        /* 容错 */
    }

    /* [6] 命令分发往返 */
    hw_core_init(NULL);
    CORE_CHECK(hw_core_cmd("count", NULL) == (int)HW_CORE_MAX);
    CORE_CHECK(hw_core_cmd("ok", NULL) == 1);
    CORE_CHECK(hw_core_cmd("idx 0", NULL) == 0x0DEF);
    CORE_CHECK(hw_core_cmd("idx 9", NULL) == 0x0FFD);
    CORE_CHECK(hw_core_cmd("idx 10", NULL) == -1);
    CORE_CHECK(hw_core_cmd("idx -1", NULL) == -1);
    CORE_CHECK(hw_core_cmd("slot 3 238", NULL) == 0);
    CORE_CHECK(hw_core_cmd("free 3", NULL) == 0);
    CORE_CHECK(hw_core_cmd("mode", NULL) == (int)hw_core_mode());
    CORE_CHECK(hw_core_mode() < HW_CORE_MODE_MAX);
    CORE_CHECK(hw_core_cmd("bogus", NULL) == -1);
    CORE_CHECK(hw_core_cmd("help", NULL) == -2);
    CORE_CHECK(hw_core_cmd(NULL, NULL) == -1);

    hw_core_init(NULL);
    if (fails == 0) {
        snprintf(line, sizeof(line),
                 "hw_core selftest: all PASS (golden=0x%08X, mode=%s)\n",
                 (unsigned)hw_core_checksum(),
                 hw_core_mode_str(hw_core_mode()));
        if (putf) putf(line);   /* putf=NULL = 静默自检 (真机固件) */
    }
    return fails;
}

/* ============================================================
 * 6. CLI: ./xiaomo core [dna|idx N|slot N CODE|free N|slots|mode|selftest]
 * ============================================================ */
static int core_cli_puts(const char* s) { return printf("%s", s); }

int hw_core_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "card";

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_core_selftest(core_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "dna") == 0) {
        uint32_t i;
        printf("=== hw_core DNA 表 (%u 项) ===\n", (unsigned)HW_CORE_MAX);
        for (i = 0; i < HW_CORE_MAX; i++)
            printf("  [%u] %-4s 0x%04X (%u) base=%s\n", (unsigned)i,
                   g_dna_table[i].name, (unsigned)g_dna_table[i].value,
                   (unsigned)g_dna_table[i].value,
                   hw_core_base_name(g_dna_table[i].value));
        return 0;
    }
    if (strcmp(sub, "idx") == 0 && argc >= 4) {
        long idx = strtol(argv[3], NULL, 10);
        if (idx < 0 || idx >= (long)HW_CORE_MAX) {
            printf("idx 越界 (0..%u)\n", (unsigned)(HW_CORE_MAX - 1));
            return 1;
        }
        printf("DNA[%ld] = %s 0x%04X (%u)\n", idx,
               g_dna_table[idx].name, (unsigned)g_dna_table[idx].value,
               (unsigned)g_dna_table[idx].value);
        return 0;
    }
    if (strcmp(sub, "slot") == 0 && argc >= 5) {
        long slot = strtol(argv[3], NULL, 10);
        long code = strtol(argv[4], NULL, 0);
        int rc = core_slot_load((uint8_t)slot, (core_dna)code);
        printf("slot %ld = 0x%04lX -> rc=%d (used=%u)\n",
               slot, code, rc, (unsigned)core_slots_used());
        return (rc == 0) ? 0 : 1;
    }
    if (strcmp(sub, "free") == 0 && argc >= 4) {
        long slot = strtol(argv[3], NULL, 10);
        int rc = core_slot_free((uint8_t)slot);
        printf("free %ld -> rc=%d (used=%u)\n",
               slot, rc, (unsigned)core_slots_used());
        return (rc == 0) ? 0 : 1;
    }
    if (strcmp(sub, "slots") == 0) {
        uint32_t i;
        printf("slots used = %u\n", (unsigned)core_slots_used());
        for (i = 0; i < HW_CORE_MAX; i++) {
            if (g_slots[i] != 0u) {
                const char* nm = "?";
                uint32_t j;
                for (j = 0; j < HW_CORE_MAX; j++)
                    if (g_dna_table[j].value == g_slots[i]) nm = g_dna_table[j].name;
                printf("  [%u] = 0x%04X (%s)\n", (unsigned)i,
                       (unsigned)g_slots[i], nm);
            }
        }
        return 0;
    }
    if (strcmp(sub, "mode") == 0) {
        printf("mode = %s (%u)\n", hw_core_mode_str(hw_core_mode()),
               (unsigned)hw_core_mode());
        return 0;
    }
    if (strcmp(sub, "help") == 0) {
        printf("core 子命令: dna(DNA表) idx N(取编码) slot N CODE(装载)\n");
        printf("  free N(卸载) slots(槽位视图) mode selftest\n");
        return 0;
    }

    /* ---- 能力卡 (默认) ---- */
    {
        uint32_t ck = hw_core_checksum();
        uint32_t i;
        hw_core_init(NULL);
        printf("=== xiaomo hw_core 内核 DNA 编码层 ===\n");
        printf("mode       = %s (%u)\n", hw_core_mode_str(hw_core_mode()),
               (unsigned)hw_core_mode());
        printf("dna cksum  = 0x%08X %s\n", (unsigned)ck,
               (ck == HW_CORE_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
        printf("golden ok  = %d\n", hw_core_cmd("ok", NULL));
        printf("---- DNA 表 (count=%u) ----\n", (unsigned)HW_CORE_MAX);
        for (i = 0; i < HW_CORE_MAX; i++)
            printf("  [%u] %-4s 0x%04X %s\n", (unsigned)i,
                   g_dna_table[i].name, (unsigned)g_dna_table[i].value,
                   hw_core_base_name(g_dna_table[i].value));
        printf("---- 槽位装载演示 ----\n");
        core_slot_load(3, (core_dna)g_dna_table[6].value);
        printf("slot 3 = 0x%04X -> used = %u\n",
               (unsigned)g_slots[3], (unsigned)core_slots_used());
        core_slot_free(3);
        printf("free 3  -> used = %u (回到空)\n", (unsigned)core_slots_used());
        printf("VM 内核联动: kvm_run 上电自动 hw_core_init; .mo 端 hw_core(\"...\")\n");
        printf("  见 examples/hwcore_test.mo\n");
        return 0;
    }
}
