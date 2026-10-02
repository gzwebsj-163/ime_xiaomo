/*
 * xiaomo - TFT 模组硬件故障诊断层 (hw_fault, 2026-09-24)
 *
 * 诊断对象: 10 脚 SPI TFT 模组 (金逸晨模组引脚定义)
 *   1.gnd  2.rs  3.cs  4.scl  5.sda
 *   6.reset 7.vdd 8.gnd 9.led+ 10.led-
 *
 * 全跨式设计 (与 hw_token 同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_FAULT_MODE_OVERRIDE=n 强指 (KELL 内核内
 *      __linux__ 亦须 KELL 宏抢先识别)。
 *   2. 核心 (模式/引脚表/故障码/诊断扫描/校验和) 仅依赖 stdint+string,
 *      真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 底层读取走 BSP 回调: 默认 = 模拟器 (故障注入表驱动, 全平台
 *      行为一致可回归); 真机固件 hw_fault_bsp_install() 注入真实
 *      GPIO/ADC/SPI 读取, 诊断逻辑零改动。
 *   4. 黄金参考: HW_FAULT_GOLDEN = 引脚表 FNV-1a-32 (独立 Python 对拍
 *      锁定, 2026-09-24), selftest 逐项断言防未来误改破坏跨历史一致。
 *
 * 六层接入 (2026-09-24):
 *   Makefile   HW_SRCS 收编 (Makefile.linux 同补)
 *   VM 内核    OP_HW_FAULT_CALL (vm_core.c, kvm_run 上电自动 hw_fault_init)
 *   编译器     mo2kbc 内置 hw_fault("...") → OP_HW_FAULT_CALL
 *   CLI        ./xiaomo hwfault [card|scan|pin N|inject N CODE|clear|mode|selftest]
 *   示例       examples/hwfault_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh hwfault 块
 */
#include "hw_fault.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 黄金校验和: 独立 Python 对拍锁定 (2026-09-24), 勿改 */
#define HW_FAULT_GOLDEN 0x6AFD1876u

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t fl_mode_probe(void)
{
#if defined(HW_FAULT_MODE_OVERRIDE)
    return (uint8_t)HW_FAULT_MODE_OVERRIDE;
#elif defined(HW_FAULT_KELL)
    return (uint8_t)HW_FAULT_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_FAULT_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_FAULT_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_FAULT_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_FAULT_MODE_HOST;
#else
    return (uint8_t)HW_FAULT_MODE_TEST;
#endif
}

uint8_t hw_fault_mode(void)
{
    static uint8_t cached = 0xFFu;   /* 0xFF = 未探测 */
    if (cached == 0xFFu) cached = fl_mode_probe();
    return cached;
}

static const char* const fl_mode_names[HW_FAULT_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_fault_mode_str(uint8_t mode)
{
    return (mode < HW_FAULT_MODE_MAX) ? fl_mode_names[mode] : "?";
}

/* ============================================================
 * 2. 引脚诊断表 (金逸晨 10 脚 SPI TFT)
 * ============================================================ */
static const hw_fault_pin_t g_pin_table[HW_FAULT_PIN_MAX] = {
    { 1,  "GND",   HW_FAULT_CLS_GND,  0 },
    { 2,  "RS",    HW_FAULT_CLS_CTRL, 0 },
    { 3,  "CS",    HW_FAULT_CLS_CTRL, 0 },
    { 4,  "SCL",   HW_FAULT_CLS_DATA, 0 },
    { 5,  "SDA",   HW_FAULT_CLS_DATA, 0 },
    { 6,  "RESET", HW_FAULT_CLS_CTRL, 0 },
    { 7,  "VDD",   HW_FAULT_CLS_PWR,  1 },
    { 8,  "GND",   HW_FAULT_CLS_GND,  0 },
    { 9,  "LED+",  HW_FAULT_CLS_LED,  0 },
    { 10, "LED-",  HW_FAULT_CLS_LED,  0 }
};

const hw_fault_pin_t* hw_fault_pin(uint8_t pin)
{
    uint32_t i;
    for (i = 0; i < HW_FAULT_PIN_MAX; i++)
        if (g_pin_table[i].pin == pin) return &g_pin_table[i];
    return NULL;
}

/* FNV-1a-32 over (name 字节串 + cls + level) 逐脚折叠 — 与 Python 对拍 */
uint32_t hw_fault_pin_checksum(void)
{
    uint32_t h = 0x811C9DC5u;
    uint32_t i;
    for (i = 0; i < HW_FAULT_PIN_MAX; i++) {
        const hw_fault_pin_t* p = &g_pin_table[i];
        const unsigned char* s = (const unsigned char*)p->name;
        while (*s) { h ^= (uint32_t)*s++; h = (h * 0x01000193u) & 0xFFFFFFFFu; }
        h ^= (uint32_t)p->cls;   h = (h * 0x01000193u) & 0xFFFFFFFFu;
        h ^= (uint32_t)p->level; h = (h * 0x01000193u) & 0xFFFFFFFFu;
    }
    return h;
}

/* ============================================================
 * 3. 故障码文本
 * ============================================================ */
static const char* const fl_cls_names[5] = {
    "GND", "PWR", "LED", "CTRL", "DATA"
};

const char* hw_fault_cls_name(uint8_t cls)
{
    return (cls <= 4) ? fl_cls_names[cls] : "?";
}

static const char* const fl_code_names[8] = {
    "NONE", "GND_OPEN", "VDD_LOW", "VDD_HIGH",
    "LED_OPEN", "LED_SHORT", "SPI_NOACK", "CTRL_STUCK"
};
static const char* const fl_code_descs[8] = {
    "no fault", "ground open", "VDD undervoltage", "VDD overvoltage",
    "backlight open", "backlight short", "SPI no ACK", "control line stuck"
};

const char* hw_fault_code_name(uint8_t code)
{
    if (code == HW_FAULT_UNPLUGGED) return "UNPLUGGED";
    if (code == HW_FAULT_UNKNOWN)   return "UNKNOWN";
    return (code <= 7) ? fl_code_names[code] : "?";
}

const char* hw_fault_code_desc(uint8_t code)
{
    if (code == HW_FAULT_UNPLUGGED) return "module not connected (VDD no power)";
    if (code == HW_FAULT_UNKNOWN)   return "unknown fault";
    return (code <= 7) ? fl_code_descs[code] : "?";
}

/* ============================================================
 * 4. BSP (真机回调) + 故障注入 (模拟器)
 * ============================================================ */
static hw_fault_bsp_t g_bsp;               /* 全 0 = 模拟器 */
static uint8_t        g_inject[HW_FAULT_PIN_MAX];   /* 每脚注入故障码, 0=NONE */
static uint8_t        g_inject_flag;       /* 注入表是否非空 (VDD 模拟依据) */

void hw_fault_init(void* arg)
{
    (void)arg;
    /* ⚠️ 有意「不」清 g_bsp (2026-10-02 修, 与 hw_dc / hw_pin / hw_flash / hw_wdbg 同款):
     *   BSP = 真机 GPIO/ADC 读回调绑定, 属「环境」而非本模块的内部状态。
     *   上电复位只清**故障注入表**(旧注入不跨运行残留), 硬件绑定保持。
     *   真机陷阱: kvm_run 上电会自动调 hw_fault_init (vm_core.c:463) ——
     *   若此处 memset(&g_bsp) 则真机装好的读回调被悄悄清空, 退回注入表,
     *   scan/pin 照样给出「健康/故障」结论, 但压根没碰硬件 (典型假成功)。
     *   卸载硬件只走 hw_fault_bsp_install(NULL)。
     *   回归证据: selftest [8] 断言 init 后 BSP 仍在。 */
    memset(g_inject, 0, sizeof(g_inject));
    g_inject_flag = 0;
}

void hw_fault_hook(void* arg) { (void)arg; }   /* 无动态资源 */

void hw_fault_bsp_install(const hw_fault_bsp_t* bsp)
{
    if (bsp) g_bsp = *bsp;
}

int hw_fault_inject(uint8_t pin, uint8_t code)
{
    if (hw_fault_pin(pin) == NULL) return -1;
    g_inject[pin - 1] = code;
    g_inject_flag = 1;
    return 0;
}

void hw_fault_clear_all(void)
{
    memset(g_inject, 0, sizeof(g_inject));
    g_inject_flag = 0;
}

/* ---- 模拟器 BSP (默认): 注入表驱动, 全平台行为一致 ---- */
static int fl_gpio_read(int pin)
{
    const hw_fault_pin_t* p = hw_fault_pin((uint8_t)pin);
    if (!p || pin < 1 || pin > (int)HW_FAULT_PIN_MAX) return -1;
    switch (g_inject[pin - 1]) {
    case HW_FAULT_GND_OPEN:  return (p->cls == HW_FAULT_CLS_GND) ? 1 : 0;
    case HW_FAULT_LED_SHORT: return (p->cls == HW_FAULT_CLS_LED) ? 1 : 0;
    case HW_FAULT_CTRL_STUCK: return 0;
    default:                 return (int)p->level;
    }
}
static int fl_adc_read_mv(int pin)
{
    const hw_fault_pin_t* p = hw_fault_pin((uint8_t)pin);
    if (!p || pin < 1 || pin > (int)HW_FAULT_PIN_MAX) return -1;
    if (g_inject_flag && g_inject[6] != 0) {   /* pin7 (VDD) 注入 */
        switch (g_inject[6]) {
        case HW_FAULT_VDD_LOW:    return 2100;
        case HW_FAULT_VDD_HIGH:   return 6200;
        case HW_FAULT_UNPLUGGED:  return 0;
        default: break;
        }
    }
    return 3300;   /* 默认 3.3V */
}
static int fl_spi_probe(int pin)
{
    if (pin < 1 || pin > (int)HW_FAULT_PIN_MAX) return -1;
    return (g_inject[pin - 1] == HW_FAULT_SPI_NOACK) ? 0 : 1;
}
static int fl_gpio_flip(int pin)
{
    const hw_fault_pin_t* p = hw_fault_pin((uint8_t)pin);
    if (!p || pin < 1 || pin > (int)HW_FAULT_PIN_MAX) return -1;
    if (g_inject[pin - 1] == HW_FAULT_CTRL_STUCK) return 0;
    if (g_inject[pin - 1] == HW_FAULT_LED_OPEN)   return 0;
    return 1;
}

/* ---- 生效 BSP: 真机回调优先, 否则模拟器 ---- */
/* selftest 专用 stub: 只用来占位一个「已装 BSP」的非空槽位, 不参与判定逻辑 */
static int fl_stub_gpio_read(int pin) { (void)pin; return 0; }
static int bsp_gpio_read(int pin)
{
    return g_bsp.gpio_read ? g_bsp.gpio_read(pin) : fl_gpio_read(pin);
}
static int bsp_adc_read_mv(int pin)
{
    return g_bsp.adc_read_mv ? g_bsp.adc_read_mv(pin) : fl_adc_read_mv(pin);
}
static int bsp_spi_probe(int pin)
{
    return g_bsp.spi_probe ? g_bsp.spi_probe(pin) : fl_spi_probe(pin);
}
static int bsp_gpio_flip(int pin)
{
    return g_bsp.gpio_flip ? g_bsp.gpio_flip(pin) : fl_gpio_flip(pin);
}

/* ============================================================
 * 5. 单脚诊断 / 全模组扫描
 * ============================================================ */
uint8_t hw_fault_pin_diag(uint8_t pin, hw_fault_report_t* out)
{
    const hw_fault_pin_t* p = hw_fault_pin(pin);
    uint8_t code = HW_FAULT_NONE;
    if (!p) return HW_FAULT_UNKNOWN;
    if (out) { out->pin = pin; out->name = p->name; }
    switch (p->cls) {
    case HW_FAULT_CLS_GND:
        /* 地线应稳定读到低; 读不到低 → 断开 */
        code = (bsp_gpio_read((int)pin) != 0) ? HW_FAULT_GND_OPEN : HW_FAULT_NONE;
        break;
    case HW_FAULT_CLS_PWR:
        /* VDD 电压窗口 2.7V..5.5V; 0V = 未上电 (模组未接入) */
        {
            int mv = bsp_adc_read_mv((int)pin);
            if (mv <= 0)            code = HW_FAULT_UNPLUGGED;
            else if (mv < 2700)     code = HW_FAULT_VDD_LOW;
            else if (mv > 5500)     code = HW_FAULT_VDD_HIGH;
            else                    code = HW_FAULT_NONE;
        }
        break;
    case HW_FAULT_CLS_LED:
        /* LED+: 输出翻转卡死 → 开路; LED-: 读到高 → 短路直通 */
        if (bsp_gpio_flip((int)pin) != 1) { code = HW_FAULT_LED_OPEN; break; }
        if (pin == 10 && bsp_gpio_read(10) != 0) { code = HW_FAULT_LED_SHORT; break; }
        code = HW_FAULT_NONE;
        break;
    case HW_FAULT_CLS_CTRL:
        /* RS/CS/RESET: 输出翻转回读, 卡死 = 控制线故障 */
        code = (bsp_gpio_flip((int)pin) == 1) ? HW_FAULT_NONE : HW_FAULT_CTRL_STUCK;
        break;
    case HW_FAULT_CLS_DATA:
        /* SCL/SDA: SPI 命令应答探测 (按线, 精确到注入/断线那根) */
        code = (bsp_spi_probe((int)pin) == 0) ? HW_FAULT_SPI_NOACK : HW_FAULT_NONE;
        break;
    default:
        code = HW_FAULT_UNKNOWN;
        break;
    }
    if (out) out->code = code;
    return code;
}

uint8_t hw_fault_scan(hw_fault_report_t* out, uint32_t cap)
{
    uint8_t faults = 0;
    uint32_t i;
    for (i = 0; i < HW_FAULT_PIN_MAX; i++) {
        hw_fault_report_t r;
        uint8_t pin = g_pin_table[i].pin;
        uint8_t c = hw_fault_pin_diag(pin, &r);
        r.level = (c == HW_FAULT_NONE) ? 0 : ((c == HW_FAULT_UNPLUGGED) ? 1 : 2);
        if (c != HW_FAULT_NONE) faults++;
        if (out && i < cap) out[i] = r;
    }
    return faults;
}

/* ============================================================
 * 6. 命令分发 (VM OP_HW_FAULT_CALL / CLI 一次性)
 *    返回: >=0 结果; -1 未识别/参数非法; -2 help
 * ============================================================ */
int hw_fault_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (!cmd) return -1;
    if (strcmp(cmd, "scan") == 0)
        return (int)hw_fault_scan(NULL, 0);
    if (strcmp(cmd, "clear") == 0) { hw_fault_clear_all(); return 0; }
    if (strcmp(cmd, "mode") == 0)  return (int)hw_fault_mode();
    if (strcmp(cmd, "help") == 0)  return -2;
    if (strncmp(cmd, "pin ", 4) == 0) {
        hw_fault_report_t r;
        long pin = strtol(cmd + 4, NULL, 10);
        if (pin < 1 || pin > (long)HW_FAULT_PIN_MAX) return -1;
        return (int)hw_fault_pin_diag((uint8_t)pin, &r);
    }
    if (strncmp(cmd, "inject ", 7) == 0) {
        long pin = 0, code = -1;
        if (sscanf(cmd + 7, "%ld %ld", &pin, &code) != 2) return -1;
        if (pin < 1 || pin > (long)HW_FAULT_PIN_MAX ||
            code < 0 || code > 255) return -1;
        return hw_fault_inject((uint8_t)pin, (uint8_t)code);
    }
    return -1;
}

/* ============================================================
 * 7. 自检 (全跨式黄金锁定)
 * ============================================================ */
#define FL_CHECK(cond) do { if (!(cond)) fails++; } while (0)

int hw_fault_selftest(int (*putf)(const char*))
{
    static const struct { uint8_t pin; uint8_t code; } cases[] = {
        { 1, HW_FAULT_GND_OPEN },   { 2, HW_FAULT_CTRL_STUCK },
        { 3, HW_FAULT_CTRL_STUCK }, { 4, HW_FAULT_SPI_NOACK },
        { 5, HW_FAULT_SPI_NOACK },  { 6, HW_FAULT_CTRL_STUCK },
        { 7, HW_FAULT_VDD_LOW },    { 7, HW_FAULT_VDD_HIGH },
        { 9, HW_FAULT_LED_OPEN },   { 10, HW_FAULT_LED_SHORT }
    };
    int fails = 0;
    uint32_t i;
    char line[160];
    hw_fault_bsp_t saved_bsp = g_bsp;

    /* [0] BSP 解耦: selftest 验证的是模块逻辑 = 模拟器语义 (注入表驱动)。
     * 真机 BSP 注入后, 注入对真实 GPIO/ADC 读取不可见 → 模拟器假设用例
     * 失效 = 真机 13-fails 根因 ([5]×10+[6]×1+[7]×2 实测吻合)。
     * 故 selftest 期间临时卸载 BSP、结束原样恢复; 真机健康度由
     * hw_fault_scan (真实 BSP) 独立负责, 两者不混。宿主无 BSP = 空表
     * 保存恢复, 行为零变化 (全平台一致, 家族纪律)。 */
    memset(&g_bsp, 0, sizeof(g_bsp));

    /* [1] 黄金校验和锁定 */
    FL_CHECK(hw_fault_pin_checksum() == HW_FAULT_GOLDEN);

    /* [2] 全 10 脚在表内且表序 = 物理序 1..10 */
    for (i = 1; i <= HW_FAULT_PIN_MAX; i++) {
        const hw_fault_pin_t* p = hw_fault_pin((uint8_t)i);
        FL_CHECK(p && p->pin == (uint8_t)i);
    }

    /* [3] 故障码文本名 */
    FL_CHECK(strcmp(hw_fault_code_name(HW_FAULT_GND_OPEN), "GND_OPEN") == 0);
    FL_CHECK(strcmp(hw_fault_code_name(HW_FAULT_UNPLUGGED), "UNPLUGGED") == 0);
    FL_CHECK(strcmp(hw_fault_code_name(HW_FAULT_UNKNOWN), "UNKNOWN") == 0);

    /* [4] 健康态扫描 = 0 故障 (模拟器干净) */
    hw_fault_clear_all();
    FL_CHECK(hw_fault_scan(NULL, 0) == 0);

    /* [5] 逐码注入 → 单脚诊断命中 (覆盖全类别) */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hw_fault_report_t r;
        hw_fault_clear_all();
        if (hw_fault_inject(cases[i].pin, cases[i].code) != 0) { fails++; continue; }
        FL_CHECK(hw_fault_pin_diag(cases[i].pin, &r) == cases[i].code);
    }

    /* [6] 多点注入 → scan 计数 */
    hw_fault_clear_all();
    hw_fault_inject(1, HW_FAULT_GND_OPEN);
    hw_fault_inject(7, HW_FAULT_VDD_LOW);
    FL_CHECK(hw_fault_scan(NULL, 0) == 2);

    /* [7] 命令分发往返 */
    hw_fault_clear_all();
    FL_CHECK(hw_fault_cmd("scan", NULL) == 0);
    hw_fault_inject(4, HW_FAULT_SPI_NOACK);
    FL_CHECK(hw_fault_cmd("pin 4", NULL) == HW_FAULT_SPI_NOACK);
    FL_CHECK(hw_fault_cmd("inject 2 7", NULL) == 0);
    FL_CHECK(hw_fault_cmd("pin 2", NULL) == HW_FAULT_CTRL_STUCK);
    hw_fault_clear_all();
    FL_CHECK(hw_fault_cmd("bogus", NULL) == -1);

    /* [8] 🔴 A-5 同款: init 不得抹掉已装的 BSP
     *   与 hw_dc / hw_pin / hw_flash 同一类的「头号假成功陷阱」:
     *   kvm_run 上电会自动调 hw_fault_init (vm_core.c:463), 若此处
     *   memset(&g_bsp) 则真机固件装好的 GPIO/ADC 读回调被悄悄清空,
     *   退回故障注入表 —— 界面照样出「健康/故障」结论, 但压根没碰硬件。
     *   契约: init 只复位**本模块**状态(注入表), 硬件绑定只由
     *   hw_fault_bsp_install() 改, 卸载 = 传 NULL。 */
    {
        hw_fault_bsp_t probe;
        memset(&probe, 0, sizeof(probe));
        probe.gpio_read = fl_stub_gpio_read;   /* 只装一个非空回调即可判别 */
        g_bsp = probe;
        hw_fault_init(NULL);                   /* 模拟 kvm_run 上电自动调用 */
        FL_CHECK(g_bsp.gpio_read != NULL);
        g_bsp = saved_bsp;                     /* 还原本用例开始前的现场 */
    }

    hw_fault_clear_all();
    g_bsp = saved_bsp;   /* 恢复 BSP (真机回调/宿主空表通用) */
    if (fails == 0) {
        snprintf(line, sizeof(line),
                 "hw_fault selftest: all PASS (golden=0x%08X, mode=%s)\n",
                 (unsigned)hw_fault_pin_checksum(),
                 hw_fault_mode_str(hw_fault_mode()));
        if (putf) putf(line);   /* putf=NULL = 静默自检 (真机固件) */
    }
    return fails;
}

/* ============================================================
 * 8. CLI: ./xiaomo hwfault [card|scan|pin N|inject N CODE|clear|mode|selftest]
 * ============================================================ */
static int fl_cli_puts(const char* s) { return printf("%s", s); }

int hw_fault_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "card";

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_fault_selftest(fl_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "scan") == 0) {
        hw_fault_report_t rep[HW_FAULT_PIN_MAX];
        uint8_t n = hw_fault_scan(rep, HW_FAULT_PIN_MAX);
        uint32_t i;
        printf("=== hw_fault scan (%u faults) ===\n", (unsigned)n);
        for (i = 0; i < HW_FAULT_PIN_MAX; i++) {
            const hw_fault_pin_t* p = hw_fault_pin(rep[i].pin);
            printf("  P%-2u %-5s cls=%-4s lvl=%u %-10s %s\n",
                   (unsigned)rep[i].pin, rep[i].name,
                   p ? hw_fault_cls_name(p->cls) : "?",
                   (unsigned)rep[i].level,
                   hw_fault_code_name(rep[i].code),
                   hw_fault_code_desc(rep[i].code));
        }
        return 0;
    }
    if (strcmp(sub, "pin") == 0 && argc >= 4) {
        hw_fault_report_t r = { 0, 0, 0, NULL };
        long pin = strtol(argv[3], NULL, 10);
        uint8_t c = hw_fault_pin_diag((uint8_t)pin, &r);
        printf("P%ld %-5s -> %s (0x%02X): %s\n", pin, r.name ? r.name : "?",
               hw_fault_code_name(c), (unsigned)c, hw_fault_code_desc(c));
        return (c == HW_FAULT_NONE) ? 0 : 1;
    }
    if (strcmp(sub, "inject") == 0 && argc >= 5) {
        long pin  = strtol(argv[3], NULL, 10);
        long code = strtol(argv[4], NULL, 0);
        int rc = hw_fault_inject((uint8_t)pin, (uint8_t)code);
        printf("inject P%ld = %s (0x%02X) -> rc=%d\n", pin,
               hw_fault_code_name((uint8_t)code), (unsigned)code, rc);
        return (rc == 0) ? 0 : 1;
    }
    if (strcmp(sub, "clear") == 0) {
        hw_fault_clear_all();
        printf("inject table cleared\n");
        return 0;
    }
    if (strcmp(sub, "mode") == 0) {
        printf("mode = %s (%u)\n", hw_fault_mode_str(hw_fault_mode()),
               (unsigned)hw_fault_mode());
        return 0;
    }
    if (strcmp(sub, "help") == 0) {
        printf("hwfault 子命令: card(能力卡) scan(全模组扫描) pin N(单脚诊断)\n");
        printf("  inject N CODE(故障注入, CODE 见 hw_fault_code_name) clear mode selftest\n");
        return 0;
    }

    /* ---- 能力卡 (默认) ---- */
    {
        hw_fault_report_t rep[HW_FAULT_PIN_MAX];
        uint8_t n = hw_fault_scan(rep, HW_FAULT_PIN_MAX);
        uint32_t i;
        uint32_t ck = hw_fault_pin_checksum();
        printf("=== xiaomo hw_fault TFT 模组故障诊断层 ===\n");
        printf("mode            = %s (%u)\n", hw_fault_mode_str(hw_fault_mode()),
               (unsigned)hw_fault_mode());
        printf("pin table cksum = 0x%08X %s\n", (unsigned)ck,
               (ck == HW_FAULT_GOLDEN) ? "(golden OK)" : "(MISMATCH!)");
        printf("BSP             = %s\n", g_bsp.gpio_read ? "真机回调已注入" : "模拟器 (故障注入表)");
        printf("---- 10 脚诊断表 (健康态 scan=%u faults) ----\n", (unsigned)n);
        for (i = 0; i < HW_FAULT_PIN_MAX; i++) {
            const hw_fault_pin_t* p = hw_fault_pin(rep[i].pin);
            printf("  P%-2u %-5s %-5s -> %s\n",
                   (unsigned)rep[i].pin, rep[i].name,
                   p ? hw_fault_cls_name(p->cls) : "?",
                   hw_fault_code_name(rep[i].code));
        }
        printf("---- 故障注入演示 ----\n");
        hw_fault_clear_all();
        hw_fault_inject(7, HW_FAULT_VDD_LOW);
        printf("inject P7=VDD_LOW -> scan = %u (VDD_LOW)\n",
               (unsigned)hw_fault_scan(NULL, 0));
        hw_fault_clear_all();
        hw_fault_inject(4, HW_FAULT_SPI_NOACK);
        printf("inject P4=SPI_NOACK -> scan = %u (SPI_NOACK)\n",
               (unsigned)hw_fault_scan(NULL, 0));
        hw_fault_clear_all();
        printf("clear -> scan = %u (健康)\n", (unsigned)hw_fault_scan(NULL, 0));
        printf("VM 内核联动: kvm_run 上电自动 hw_fault_init; .mo 端 hw_fault(\"...\")\n");
        printf("  见 examples/hwfault_test.mo; 真机: hw_fault_bsp_install() 注入读取回调\n");
        return 0;
    }
}
