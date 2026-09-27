/*
 * xiaomo - 跨模式域令牌层 (hw_token)
 *
 * 职责: 给 xiaomo 全家桶的 10 个「运行域」发放可验证的 64 位域令牌,
 *       并提供跨一切模式的封印/解封通道。核心铁律:
 *
 *   全跨模式 (cross-mode bit-exact):
 *     同一域的令牌值在 宿主(macOS/Win) / Linux(kickpi/server/Termux) /
 *     内核嵌入(KELL/TinyEMU 真内核) / ESP32 / ESP8266 / TEST 六种模式下,
 *     无论 gcc/clang/g++/C/C++ 编译, 派生结果必须逐位一致。
 *     实现保证: 派生只用无符号 64 位整数的移位/乘法/异或 (溢出按模 2^64
 *     回绕, C 标准对无符号溢出有明确定义), 不用 rand/time/指针/浮点/
 *     大端假设, freestanding 可编译 (仅依赖 stdint.h)。
 *
 *   域盐 HW_TOKEN_DOMAIN = ASCII "XIAOMO01" (0x5849414F4D4F3031),
 *   与 hw_oem 熔丝签名同源 —— 令牌是设备的「域身份证」。
 *   混淆掩码 0x5A (HW_TOKEN_MASK) 沿用私有 IR 的 XOR 0x5A 传统。
 *
 *   黄金参考表 (HW_TOKEN_GOLDEN): 首版派生值由独立 C 实现与 Python
 *   实现对拍锁定 (2026-09-13), 运行期派生必须逐位等于此表,
 *   selftest 与 CLI 均做该断言。
 */
#ifndef HW_TOKEN_H
#define HW_TOKEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. 常量: 域盐 / 掩码 / 线格式容量
 * ============================================================ */

/* 域盐: ASCII "XIAOMO01", 与 hw_oem 熔丝同一枚身份常量 */
#define HW_TOKEN_DOMAIN     0x5849414F4D4F3031ULL
/* 混淆掩码: XOR 0x5A 全宽展开 (私有 IR 传统) */
#define HW_TOKEN_MASK       0x5A5A5A5A5A5A5A5AULL

/* seal 文本线格式最大长度 (含结尾 NUL):
 * "TK1.<t>.<m>.<seq>.<16hex>.<8hex>" 最长 ~47 字节, 64 留余量 */
#define HW_TOKEN_STR_CAP    64

/* ============================================================
 * 2. 令牌类型 = 跨模式运行域 (10 域, 序号即派生序, 不得变更)
 * ============================================================ */

typedef enum {
    HW_KEY_TOKEN     = 0x00,   /* KEY   根域: 派生体系的根 */
    HW_OEM_TOKEN     = 0x01,   /* OEM   设备身份域 (hw_oem 熔丝同源) */
    HW_HWD_TOKEN     = 0x02,   /* HWD   设备命令分发域 (hw_dev) */
    HW_POINT_TOKEN   = 0x03,   /* POINT 探针/测量域 (ADC/GPIO/充电监测) */
    HW_SELF_TOKEN    = 0x04,   /* SELF  自宿主域 (VM 承载自身) */
    HW_KELL_TOKEN    = 0x05,   /* KELL  内核嵌入域 (TinyEMU 真内核) */
    HW_LINUX_TOKEN   = 0x06,   /* LINUX Linux 域 (kickpi/server/Termux) */
    HW_ESP32_TOKEN   = 0x07,   /* ESP32 ESP32-S3/C3/C6 真机域 */
    HW_ESP8266_TOKEN = 0x08,   /* ESP8266 ESP8266 真机域 */
    HW_TEST_TOKEN    = 0x09,   /* TEST  测试域 (对拍/回归专用) */

    HW_TOKEN_TYPE_MAX = 0x0A  /* 域总数 (非法类型 >= 此值) */
} hw_token_type_t;

/* ============================================================
 * 3. 跨模式运行模式 (编译期探测, 运行期可显式指定)
 * ============================================================ */

typedef enum {
    HW_TOKEN_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_TOKEN_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux/树莓派) */
    HW_TOKEN_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_TOKEN_MODE_ESP32   = 3,   /* ESP32-S3/C3/C6 (IDF) */
    HW_TOKEN_MODE_ESP8266 = 4,   /* ESP8266 (ESP8266_RTOS/NonOS) */
    HW_TOKEN_MODE_TEST    = 5,   /* 测试模式 (未知平台兜底) */

    HW_TOKEN_MODE_MAX     = 6
} hw_token_mode_t;

/* mode 检测开关:
 *   - 宿主/ESP 由编译宏自动探测 (见实现);
 *   - KELL 内核内编译亦为 __linux__, 需在 KELL 构建系统加
 *     -DHW_TOKEN_KELL=1 才会被识别为 KELL 模式;
 *   - 任何构建都可用 -DHW_TOKEN_MODE_OVERRIDE=n 强指模式。
 *     (override 属于构建期决策, 不破坏跨模式派生一致性) */

/* 令牌标志位 (16 位) */
#define HW_TOKEN_F_NONE     0x0000  /* 原生签发 */
#define HW_TOKEN_F_SEALED   0x0001  /* 经 seal/open 通道流转 */
#define HW_TOKEN_F_STICKY   0x0002  /* 常驻令牌 (issue 不递增 seq) */

/* ============================================================
 * 4. 令牌对象
 * ============================================================ */

typedef struct hw_token {
    uint8_t  type;    /* hw_token_type_t: 所属域 */
    uint8_t  mode;    /* hw_token_mode_t: 签发时所在模式 */
    uint16_t flags;   /* HW_TOKEN_F_* */
    uint32_t seq;     /* 同域签发序号, 单调递增 (从 1 起) */
    uint64_t value;   /* 域令牌值 == hw_token_derive(type), 只读字段 */
} hw_token_t;

/* ============================================================
 * 5. API
 * ============================================================ */

/* 域令牌派生 (核心原语): 纯无符号整型运算, 全模式逐位一致。
 * type 非法 (>= HW_TOKEN_TYPE_MAX) 返回 0。 */
uint64_t        hw_token_derive(uint8_t type);

/* 编译期探测的运行模式 (HW_TOKEN_MODE_* 枚举值) */
uint8_t         hw_token_mode(void);

/* 域/模式/标志的字符串形式 ("KEY"/"HOST"/"SEALED"...; 未知返回 "??") */
const char*     hw_token_type_str(uint8_t type);
const char*     hw_token_mode_str(uint8_t mode);
const char*     hw_token_flags_str(uint16_t flags);   /* 多标志 "|" 相连 */

/* 签发: 填 *out (type/mode/flags/seq/value), seq 每签发 +1。
 * type 非法或 out 为空返回 -1; STICKY 域不递增 seq。 */
int             hw_token_issue(uint8_t type, hw_token_t* out);

/* 验证: value == derive(type) 且 type/mode 合法返回 0, 否则 -1。
 * (篡改 value 或 type 都会被识破) */
int             hw_token_verify(const hw_token_t* tk);

/* 指定域当前已签发总次数 (跨签发累计) */
uint32_t        hw_token_seq(uint8_t type);

/* ---- 跨模式封印/解封 (文本通道, 串口/网络/剪贴板皆可传) ----
 * seal : token -> "TK1.<t>.<m>.<seq>.<value16hex>.<cksum8hex>"
 *        (cksum = FNV-1a-32, 覆盖前缀全部字段)
 * open : 反解 + 三重验证 (结构/域值/校验和), 成功回填 *out 并置
 *        F_SEALED 位; 任一不符返回 -1。
 * issue 失败 (type 非法) 或 cap 不足时 seal 返回 -1。 */
int             hw_token_seal(uint8_t type, uint16_t flags,
                              char* out, uint32_t cap);
int             hw_token_open(const char* s, hw_token_t* out);

/* ---- 自检与 CLI ---- */

/* 自检输出回调: 传 NULL = 静默模式 (仅取返回值) */
typedef int (*hw_token_puts_fn)(const char* s);

/* 自检: 派生==黄金表(10域)/签发/验证/篡改拒绝/序号递增/seal-open
 * 回环/越权 seal 拒解/坏串拒开/非法域派生为0。
 * 返回失败项数 (0 = 全绿; 当前共 19 项)。 */
int             hw_token_selftest(hw_token_puts_fn putf);

/* CLI: ./xiaomo token —— 全域令牌卡 + seal/open 回环 + 自检 */
int             hw_token_cli(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HW_TOKEN_H */
