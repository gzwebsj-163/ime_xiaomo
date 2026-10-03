#ifndef HW_WDBG_H
#define HW_WDBG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - 无线调试器信号层 (hw_wdbg, 2026-09-29)
 *
 * 复刻对象: 立创开源硬件「【AI设计】AI远程调试器」
 *           (ESP32-S3 无线串口调试器 · 串口桥接 + 多协议信号监控)
 *
 * 把原项目的核心信号能力搬进 xiaomo 全跨式 hw 家族:
 *   1. 串口桥接   : 2048B 环形流缓冲 + 波特率档位白名单 +
 *                   RX/TX 字节计数 + 溢出丢弃计数
 *   2. PWM 监控   : 输出 (LEDC 语义) + 测频/占空比 (双沿语义)
 *   3. SPI 监控   : Master 传输 + Slave 捕获环形缓冲 (50 条 × 64B) + Mode 0~3
 *   4. I2C 监控   : Master 读/写 + Slave/Passive 捕获环形缓冲 (50 条 × 64B)
 *                   + 7bit 从地址 + 读/写方向
 *   5. 自定义线序 : UART/SPI/I2C/PWM 四协议引脚线序可改可复位
 *
 * 全跨式设计 (与 hw_token / hw_fault / hw_core 同款):
 *   1. 编译期模式探测六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_WDBG_MODE_OVERRIDE=n 强指。
 *   2. 核心 (模式/线序表/环形缓冲/命令面) 仅依赖 stdint+string+stdio,
 *      真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 底层收发一律走 BSP 回调: 默认 = 模拟器 (确定性回环/合成数据,
 *      全平台逐位一致可回归); 真机固件 hw_wdbg_bsp_install() 注入
 *      真实 UART/SPI/I2C/PWM, 上层逻辑零改动。
 *   4. 黄金参考: HW_WDBG_GOLDEN = 默认线序表 FNV-1a-32 (独立 Python
 *      对拍锁定, 2026-09-29), selftest 断言防未来误改。
 *
 * 六层接入 (2026-09-29):
 *   Makefile   HW_SRCS 收编 (Makefile.linux 同补)
 *   VM 内核    OP_HW_WDBG_CALL (vm_core.c, kvm_run 上电自动 hw_wdbg_init)
 *   编译器     mo2kbc 内置 hw_wdbg("...") → OP_HW_WDBG_CALL
 *   CLI        ./xiaomo wdbg [card|status|bridge|pwm|spi|i2c|pin|mode|selftest]
 *   示例       examples/wdbg_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh wdbg 块
 * ============================================================ */

/* ---- 编译期模式 (与 hw_token 同款) ---- */
typedef enum {
    HW_WDBG_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_WDBG_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux/树莓派) */
    HW_WDBG_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_WDBG_MODE_ESP32   = 3,   /* ESP32-S3/C3/C6 (IDF) */
    HW_WDBG_MODE_ESP8266 = 4,   /* ESP8266 (RTOS/NonOS) */
    HW_WDBG_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_WDBG_MODE_MAX     = 6
} hw_wdbg_mode_t;

/* ---- 被监控总线协议 ---- */
typedef enum {
    HW_WDBG_PROTO_UART = 0,   /* 串口桥接 */
    HW_WDBG_PROTO_SPI  = 1,   /* SPI 总线 */
    HW_WDBG_PROTO_I2C  = 2,   /* I2C 总线 */
    HW_WDBG_PROTO_PWM  = 3,   /* PWM 信号 */
    HW_WDBG_PROTO_MAX  = 4
} hw_wdbg_proto_t;

/* ---- 返回码 (非 0 即失败, -1/-2 与 hw_core 约定一致) ---- */
#define HW_WDBG_R_OK         0x00  /* 成功 */
#define HW_WDBG_R_NOARGS     0x01  /* 参数缺失 */
#define HW_WDBG_R_BADARG     0x02  /* 参数非法 */
#define HW_WDBG_R_IOERR      0x03  /* 底层硬件失败 */
#define HW_WDBG_R_STATE      0x04  /* 状态不符 (未打开/未启动) */
#define HW_WDBG_R_NOCMD      (-1)  /* 未识别命令 */
#define HW_WDBG_R_HELP       (-2)  /* help 已吐出 */

/* ---- 容量 (与原项目对齐) ---- */
#define HW_WDBG_STREAM_CAP  2048U  /* 串口流缓冲字节 (原件 2048B stream buffer) */
#define HW_WDBG_TRACE_MAX     50U  /* 事务环形缓冲条数 (原件最近 50 条) */
#define HW_WDBG_TRACE_BYTES   64U  /* 单条事务最大字节 (原件每条 64B) */
#define HW_WDBG_BAUD_MAX       4U  /* 波特率档位 (原件 9600/115200/460800/921600) */
#define HW_WDBG_LINE_MAX       8U  /* 单协议最多信号线 */

/* ---- 黄金校验和 (默认线序表 FNV-1a-32, 2026-09-29 Python 对拍锁定, 勿改) ---- */
#define HW_WDBG_GOLDEN      0xCD91F641u

/* ---- 线序表项 (默认表 + 活动表) ---- */
typedef struct {
    uint8_t      proto;                     /* hw_wdbg_proto_t */
    const char*  name;                      /* "UART"/"SPI"/"I2C"/"PWM" */
    const char*  lines;                     /* 信号线名 (逗号分隔) */
    uint8_t      npins;                     /* 有效引脚数 (<= HW_WDBG_LINE_MAX) */
    uint8_t      pins[HW_WDBG_LINE_MAX];    /* 引脚号 GPIO */
    uint32_t     rate;                      /* 默认速率: UART baud / SPI Hz / I2C Hz / PWM Hz */
} hw_wdbg_pin_t;

/* ---- 事务记录 (SPI/I2C 捕获, 原项目 50×64B 环形缓冲) ---- */
typedef struct {
    uint32_t ts_us;                        /* 时间戳 (us; 模拟器=确定性递增) */
    uint8_t  proto;                        /* HW_WDBG_PROTO_SPI / _I2C */
    uint8_t  dir;                          /* 0=写(主→从/tx) 1=读(从→主/rx) */
    uint8_t  addr;                         /* I2C 7bit 从地址; SPI 未用=0 */
    uint8_t  mode;                         /* SPI mode 0..3; I2C 未用=0 */
    uint16_t len;                          /* 有效字节数 */
    uint8_t  data[HW_WDBG_TRACE_BYTES];    /* 事务数据 */
} hw_wdbg_trace_t;

/* ---- 状态快照 ---- */
typedef struct {
    uint32_t baud;              /* 当前波特率 (0 = 未打开) */
    uint32_t tx_bytes;          /* 写到 DUT 的字节数 */
    uint32_t rx_bytes;          /* 从 DUT 读出的字节数 */
    uint32_t stream_len;        /* 当前流缓冲待读字节 */
    uint32_t spill;             /* 流缓冲溢出丢弃字节 */
    uint32_t spi_traces;        /* SPI 捕获事务数 */
    uint32_t i2c_traces;        /* I2C 捕获事务数 */
    uint32_t pwm_hz;            /* 最近一次 PWM 测量频率 */
    uint32_t pwm_duty;          /* 最近一次 PWM 测量占空比 (千分比 0..1000) */
    uint32_t events;            /* 已处理命令数 */
} hw_wdbg_stat_t;

/* ---- 真机 BSP: 底层收发回调注入 (默认 = 确定性模拟器) ----
 * 返回约定: uart_open/close 0=成功; uart_write 返回实际写入字节 (<0 失败);
 * uart_read 返回读到字节 (0=暂无, <0 失败); pwm_out 0=成功;
 * pwm_meas 0=成功 (无信号时 *hz=0); spi_xfer/i2c_xfer 返回完成字节 (<0 失败)。 */
typedef struct {
    int (*uart_open)(uint32_t baud);
    int (*uart_close)(void);
    int (*uart_write)(const uint8_t* d, uint32_t n);
    int (*uart_read)(uint8_t* d, uint32_t cap);
    int (*pwm_out)(uint32_t hz, uint32_t duty_permille);
    int (*pwm_meas)(uint32_t* hz, uint32_t* duty_permille);
    int (*spi_xfer)(uint8_t mode, const uint8_t* tx, uint8_t* rx, uint32_t n);
    int (*i2c_xfer)(uint8_t addr7, uint8_t rd, const uint8_t* tx, uint8_t* rx, uint32_t n);
} hw_wdbg_bsp_t;

/* ============================================================
 * API
 * ============================================================ */

uint8_t     hw_wdbg_mode(void);
const char* hw_wdbg_mode_str(uint8_t mode);
const char* hw_wdbg_proto_name(uint8_t proto);
const char* hw_wdbg_result_code_str(int rc);

/* 默认线序表 (只读) 与校验和 (黄金锁定) */
const hw_wdbg_pin_t* hw_wdbg_pin_table(void);
uint32_t    hw_wdbg_pin_checksum(void);

/* 活动线序 (可被 pin set 修改) */
const hw_wdbg_pin_t* hw_wdbg_pin_active(uint8_t proto);

/* 波特率档位白名单 */
uint32_t    hw_wdbg_baud_at(uint32_t idx);      /* idx 越界返回 0 */
int         hw_wdbg_baud_ok(uint32_t baud);     /* 1=在档位内 */

/* 生命周期 (kvm_run 上电自动调 init) */
void        hw_wdbg_init(void* arg);            /* 复位全部状态位/BSP/线序 */
void        hw_wdbg_hook(void* arg);            /* 资源钩子 (当前无动态资源) */
void        hw_wdbg_bsp_install(const hw_wdbg_bsp_t* bsp);  /* NULL=还原默认模拟器 */

/* 状态快照 / 最近结果文本 */
void        hw_wdbg_stat(hw_wdbg_stat_t* out);
void        hw_wdbg_result(char* buf, uint32_t cap);

/* 捕获注入 (真机固件从 ISR/任务侧投递捕获到环形缓冲) */
int         hw_wdbg_trace_push(const hw_wdbg_trace_t* t);

/* 命令分发 (VM OP_HW_WDBG_CALL 用); 返回 >=0 结果码 / -1 未识别 / -2 help */
int         hw_wdbg_cmd(const char* cmd, void* ctx);

/* 自检 (putf=NULL 静默) → 失败项数 (0 = 全过) */
int         hw_wdbg_selftest(int (*putf)(const char*));
/* 自检失败行号留痕: 只报「挂了几条」而不报「哪一条」时, 无法执行「先怀疑用例」。
 * 模块零平台依赖, 故由平台侧 (真机 diag) 取行号后自行打印。 */
uint32_t    hw_wdbg_selftest_failline(uint32_t i);
uint32_t    hw_wdbg_selftest_failcount(void);
/* [18] 悬垂契约的 A/B 实测: 1=真拷了副本 0=只存了指针
 * [18b] 夹具有判别力的证明: 1=有缺陷实现确实被抓到 (否则 [18] 的 1 不可信) */
int         hw_wdbg_dangle_pos(void);
int         hw_wdbg_dangle_neg(void);
/* CLI: ./xiaomo wdbg [...] */
int         hw_wdbg_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* HW_WDBG_H */
