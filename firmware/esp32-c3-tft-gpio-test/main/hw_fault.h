#ifndef HW_FAULT_H
#define HW_FAULT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - TFT 模组硬件故障诊断层 (hw_fault, 2026-09-24)
 *
 * 诊断对象: 10 脚 SPI TFT 模组 (金逸晨模组引脚定义)
 *   1.gnd  2.rs  3.cs  4.scl  5.sda
 *   6.reset 7.vdd 8.gnd 9.led+ 10.led-
 *
 * 全跨式设计 (与 hw_token 同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_FAULT_MODE_OVERRIDE=n 强指。
 *   2. 核心 (模式/引脚表/故障码/诊断扫描/校验和) 仅依赖 stdint+string,
 *      真机 (ESP32/ESP8266/内核) 可 freestanding 编译。
 *   3. 底层读取走 BSP 回调: 默认 = 模拟器 (故障注入表驱动, 全平台
 *      行为一致可回归); 真机固件 hw_fault_bsp_install() 注入真实
 *      GPIO/ADC/SPI 读取函数, 诊断逻辑零改动。
 *   4. 故障注入 hw_fault_inject(pin, code): 模拟器侧强制某脚故障,
 *      selftest 逐码断言 (黄金参考 = 引脚表 FNV-1a-32 校验和锁定)。
 * ============================================================ */

/* ---- 编译期模式 (与 hw_token 同款) ---- */
typedef enum {
    HW_FAULT_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_FAULT_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux/树莓派) */
    HW_FAULT_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_FAULT_MODE_ESP32   = 3,   /* ESP32-S3/C3/C6 (IDF) */
    HW_FAULT_MODE_ESP8266 = 4,   /* ESP8266 (RTOS/NonOS) */
    HW_FAULT_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_FAULT_MODE_MAX     = 6
} hw_fault_mode_t;

/* ---- 引脚类别 ---- */
typedef enum {
    HW_FAULT_CLS_GND  = 0,   /* 地线 */
    HW_FAULT_CLS_PWR  = 1,   /* 电源 */
    HW_FAULT_CLS_LED  = 2,   /* 背光 */
    HW_FAULT_CLS_CTRL = 3,   /* 控制线 (RS/CS/RESET) */
    HW_FAULT_CLS_DATA = 4    /* 数据线 (SCL/SDA) */
} hw_fault_cls_t;

/* ---- 故障码 (DTC 风格) ---- */
enum {
    HW_FAULT_NONE       = 0x00,  /* 无故障 */
    HW_FAULT_GND_OPEN   = 0x01,  /* 地线断开 (GND 读不到低) */
    HW_FAULT_VDD_LOW    = 0x02,  /* 供电电压过低 (<2.7V) */
    HW_FAULT_VDD_HIGH   = 0x03,  /* 供电电压过高 (>5.5V) */
    HW_FAULT_LED_OPEN   = 0x04,  /* 背光开路 (LED+ 无法驱动) */
    HW_FAULT_LED_SHORT  = 0x05,  /* 背光短路 (LED- 读到高) */
    HW_FAULT_SPI_NOACK  = 0x06,  /* SPI 无应答 (SCL/SDA 链路断) */
    HW_FAULT_CTRL_STUCK = 0x07,  /* 控制线卡死 (RS/CS/RESET 无法翻转) */
    HW_FAULT_UNPLUGGED  = 0xFE,  /* 模组未接入 (VDD 无电压) */
    HW_FAULT_UNKNOWN    = 0xFF   /* 未知故障 */
};

#define HW_FAULT_PIN_MAX 10U

/* ---- 引脚描述表项 ---- */
typedef struct {
    uint8_t      pin;    /* 物理引脚号 1..10 */
    const char*  name;   /* 网络名 (GND/RS/CS/SCL/SDA/RESET/VDD/LED+/LED-) */
    uint8_t      cls;    /* hw_fault_cls_t */
    uint8_t      level;  /* 期望空闲电平 (GND=0, VDD=1, 其余 0) */
} hw_fault_pin_t;

/* ---- 诊断报告 ---- */
typedef struct {
    uint8_t      pin;
    uint8_t      code;   /* hw_fault 故障码 */
    uint8_t      level;  /* 0=OK 1=WARN 2=ERR */
    const char*  name;   /* 指向引脚表 name (非拷贝) */
} hw_fault_report_t;

/* ---- 真机 BSP: 底层读取回调注入 (默认=模拟器) ----
 * 返回约定: -1 = 不支持/读失败; gpio_read 0/1; adc_read_mv 单位 mV (0=无电);
 * spi_probe 按线探测 1=有应答 0=无 (SCL/SDA 逐线); gpio_flip 1=可翻转 0=卡死。 */
typedef struct {
    int (*gpio_read)(int pin);
    int (*adc_read_mv)(int pin);
    int (*spi_probe)(int pin);
    int (*gpio_flip)(int pin);
} hw_fault_bsp_t;

/* ---- API ---- */
uint8_t hw_fault_mode(void);
const char* hw_fault_mode_str(uint8_t mode);
const hw_fault_pin_t* hw_fault_pin(uint8_t pin);
uint32_t hw_fault_pin_checksum(void);          /* 引脚表 FNV-1a-32 (黄金锁定) */
const char* hw_fault_cls_name(uint8_t cls);    /* 引脚类别短名 */
const char* hw_fault_code_name(uint8_t code);  /* 故障码短名 */
const char* hw_fault_code_desc(uint8_t code);  /* 故障码描述 */

void hw_fault_init(void* arg);                 /* 复位注入表/BSP (kvm_run 上电调) */
void hw_fault_hook(void* arg);                 /* 资源钩子 (当前无动态资源) */
void hw_fault_bsp_install(const hw_fault_bsp_t* bsp);

int  hw_fault_inject(uint8_t pin, uint8_t code);  /* 0 成功; -1 参数非法 */
void hw_fault_clear_all(void);

uint8_t hw_fault_pin_diag(uint8_t pin, hw_fault_report_t* out); /* 单脚诊断 → 故障码 */
uint8_t hw_fault_scan(hw_fault_report_t* out, uint32_t cap);    /* 全模组扫描 → 故障数 */

int  hw_fault_cmd(const char* cmd, void* ctx);  /* 命令分发 (VM OP_HW_FAULT_CALL 用) */
int  hw_fault_selftest(int (*putf)(const char*)); /* 自检 → 失败数 (0=全过) */
int  hw_fault_cli(int argc, char** argv);        /* ./xiaomo hwfault [...] */

#ifdef __cplusplus
}
#endif
#endif
