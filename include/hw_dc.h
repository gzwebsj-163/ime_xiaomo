#ifndef HW_DC_H
#define HW_DC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - DC 电源信号层 (hw_dc, 2026-10-01)
 *
 * 定位: 把 DC 电源的「输入侧 / 输出侧信号」纳入 hw 家族 ——
 *       信号取值 -> 极值窗口判决 -> 参考预值对拍 -> 组合数据帧。
 *       典型场景: 充电监测台 (VBUS 输入侧 / 电池输出侧)、DC 电源轨监控。
 *
 * 全跨式设计 (与 hw_token/hw_fault/hw_core 家族同款):
 *   1. 编译期六模式探测 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_DC_MODE_OVERRIDE=n 强指。
 *   2. 核心 (模式/信号表/数据表/极值判决/校验和) 仅依赖
 *      stdint + string, 真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 信号采集走 BSP 回调注入:
 *        默认 = 确定性模拟 (表驱动, 全平台逐位一致可回归);
 *        真机固件 hw_dc_bsp_install() 注入真实 ADC/GPIO 读数, 判决逻辑零改动。
 *   4. 黄金参考 HW_DC_GOLDEN = 信号表 + 数据表 FNV-1a-32
 *      (独立 Python 对拍锁定, 2026-10-01), selftest 逐项断言防未来误改。
 *
 * 六层接入: Makefile / OP_HW_DC_CALL / mo2kbc 内置 / CLI dc /
 *           examples/dc_test.mo / tests/run_tests.sh
 *
 * ⚠️ 2026-10-01 整合说明 (原草案修复):
 *   - 原 `#define DC_DATA_1 …` 与 `enum dc_data { DC_DATA_1 }` 同名,
 *     预处理后枚举成员变成表达式 → 无法编译; 本版数据帧只保留宏,
 *     枚举 `dc_data` 移除 (其成员名与宏一一对应)。
 *   - 原 `dc_*` 四个函数签名 (int* 裸指针) 保留并语义修复 (判 NULL)。
 *   - 原 `.c` 里 `#if HW_FAULT_NONE` 守卫恒假 (那是枚举常量非宏)
 *     → 整个翻译单元为空, 已移除 (hw_dc 不依赖 hw_fault)。
 * ============================================================ */

/* ---- 编译期模式 (全跨式家族统一: 0=HOST .. 5=TEST) ---- */
typedef enum {
    HW_DC_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_DC_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux) */
    HW_DC_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_DC_MODE_ESP32   = 3,   /* ESP32 / S3 / C3 / C6 (IDF) */
    HW_DC_MODE_ESP8266 = 4,   /* ESP8266 */
    HW_DC_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_DC_MODE_MAX     = 6
} hw_dc_mode_t;

/* ---- 8 路 DC 信号 (用户定版: 输入/输出 + 极值 + 高低位) ---- */
typedef enum {
    DC_INPUT = 0,   /* DC 输入信号值 */
    DC_OUTPUT,      /* DC 输出信号值 */
    DC_MAXIN,       /* DC 输入信号最大值 */
    DC_MININ,       /* DC 输入信号最小值 */
    DC_MAXOUT,      /* DC 输出信号最大值 */
    DC_MINOUT,      /* DC 输出信号最小值 */
    DC_HIGH_LITE,   /* DC 最高时序/中继信号值 */
    DC_LOW_LITE,    /* DC 最低时序/中继信号值 */
    DC_SIG_COUNT
} dc_signal_t;

/* ---- 侧别 (参考预值所属) ---- */
#define DC_SIDE_IN   0U
#define DC_SIDE_OUT  1U

/* ---- 信号默认值 (用户原式, 保留) ---- */
#define DC_MACRO_INPUT          0x00U   /* DC 电源输入 MAC 信号 */
#define DC_MACRO_OUTPUT         0x00U   /* DC 电源输出 MAC 信号 */
#define DC_MACRO_MAXIN          0x00U   /* 输入信号最大值 */
#define DC_MACRO_MININ          0x00U   /* 输入信号最小值 */
#define DC_MACRO_MAXOUT         0x00U   /* 输出信号最大值 */
#define DC_MACRO_MINOUT         0x00U   /* 输出信号最小值 */
#define DC_INPUT_DATA           0U      /* DC 输入数据信号值 */
#define DC_OUTPUT_DATA          0U      /* DC 输出数据信号值 */

/* ---- 高低位 / 参考预值 (用户原式) ---- */
#define DC_MACRO_HIGH_LITE      0x01U   /* 最高时序和中继的信号值 */
#define DC_MACRO_LOW_LITE       0x02U   /* 最低时序和中继的信号值 */
#define DC_MACRO_INPUT_BASE_SIGNAL  0x059U   /* 输入侧参考预值 */
#define DC_MACRO_OUTPUT_BASE_SIGNAL 0x0080U  /* 输出侧参考预值 */

/* ---- 帧开关 / 时间 (用户原式) ---- */
#define DC_MACRO_START          0x2ef80U   /* DC 信号开启 */
#define DC_MACRO_END            0xdffedU   /* DC 信号关断 */
#define DC_MACRO_TIMES          0U         /* DC 信号时间 */
#define DC_MACRO_INPUT_TIMES    0U         /* DC 输入触发时间 */
#define DC_MACRO_OUT_TIMES      0U         /* DC 输出触发时间 */

/* ---- 默认编号 (用户原式) ---- */
#define DC_INPUT_NAME  "dc_input"    /* DC 输入默认编号 */
#define DC_OUTPUT_NAME "dc_output"   /* DC 输出默认编号 */

/* ---- 7 种数据组合帧 (用户原式: 位或组合) ---- */
#define DC_DATA_1   (DC_INPUT_DATA  | DC_MACRO_INPUT_TIMES)          /* 0x000 */
#define DC_DATA_2   (DC_OUTPUT_DATA | DC_MACRO_OUT_TIMES)            /* 0x000 */
#define DC_DATA_3   (DC_MACRO_MAXIN | DC_MACRO_MAXOUT)               /* 0x000 */
#define DC_DATA_4   (DC_MACRO_HIGH_LITE | DC_MACRO_LOW_LITE)         /* 0x003 */
#define DC_DATA_5   (DC_MACRO_INPUT_BASE_SIGNAL | DC_MACRO_OUTPUT_BASE_SIGNAL) /* 0x0D9 */
#define DC_DATA_6   (DC_MACRO_INPUT_BASE_SIGNAL  | 0x0059U)          /* 0x059 */
#define DC_DATA_7   (DC_MACRO_OUTPUT_BASE_SIGNAL | 0x0080U)          /* 0x080 */
#define DC_DATA_COUNT 7U

/* ---- 命令/操作结果码 (0=OK, >0 错误; .mo 侧 uint8 回绕) ---- */
#define HW_DC_OK       0    /* 成功 */
#define HW_DC_NOARGS   1    /* 缺参数 */
#define HW_DC_BADARG   2    /* 参数非法 */
#define HW_DC_IOERR    3    /* BSP 读失败 */
#define HW_DC_OUTRANGE 4    /* 信号越极值窗口 */
#define HW_DC_BADMODE  5    /* 模式非法 */

/* ============================================================
 * DCPP — DC 供电协议 (帧层, 2026-10-01)
 *
 * 在信号层之上加一层「字节流帧协议」, 把 DC_MACRO_START/END
 * (用户原式 0x02EF80 / 0x0DFFED) 用成帧定界符:
 *
 *   偏移  字段     字节  说明
 *   0..2  START     3    DC_MACRO_START 小端 = 80 EF 02
 *   3     VER       1    协议版本 = 0x01
 *   4     SIDE      1    0=IN / 1=OUT (GET/SET/BASE/RANGE 用)
 *   5     CMD       1    命令号 (见 DCPP_CMD_*)
 *   6     LEN       1    负载长度 0..HW_DCPP_MAX_PAYLOAD
 *   7..   PAYLOAD   LEN  命令负载
 *   +0    CKSUM8    1    FNV-1a-32(VER,SIDE,CMD,LEN,PAYLOAD) 折叠 1 字节
 *   +1    END       3    DC_MACRO_END 小端 = ED FF 0D
 *   帧长 = 11 + LEN (最小 11, 最大 27)
 *
 * 会话语义: OPEN = 供电会话开启 (对应 START 定界),
 *           CLOSE = 供电会话关断 (对应 END 定界);
 *           GET/SET/BASE/RANGE/DATA 必须在已开启会话内, 否则 DCPP_ERR_CLOSED。
 *
 * 全跨式: 纯整数算法, 无平台依赖, 六模式行为逐位一致;
 *         黄金值 HW_DCPP_GOLDEN = 命令表 + 帧常量 FNV-1a-32
 *         (独立 Python 对拍锁定, 2026-10-01)。
 * ============================================================ */

#define HW_DCPP_VER          0x01U                    /* 协议版本 */
#define HW_DCPP_HDR_LEN      7U                       /* START(3)+VER+SIDE+CMD+LEN */
#define HW_DCPP_END_LEN      3U                       /* END 标记长 */
#define HW_DCPP_CKSUM_LEN    1U                       /* 校验字节 */
#define HW_DCPP_MAX_PAYLOAD  16U                      /* 最大负载 */
#define HW_DCPP_MAX_FRAME    27U                      /* 11 + 16 */
#define HW_DCPP_GOLDEN       0x9432D6A5u              /* 协议黄金 (勿改) */
#define HW_DCPP_PONG         0x50U                    /* PING 应答字节 */

/* 命令号 (黄金折叠对象: 编号 + 名; 追加须重算黄金) */
#define DCPP_CMD_PING   0x01U   /* 存活探测 → 应答 0x50 */
#define DCPP_CMD_COUNT  0x02U   /* 能力计数 → [信号数, 数据帧数] */
#define DCPP_CMD_GET    0x03U   /* [sig] → 信号值 2B 小端 */
#define DCPP_CMD_SET    0x04U   /* [sig,val_lo,val_hi] → [rc] */
#define DCPP_CMD_BASE   0x05U   /* [side] → 参考预值 2B 小端 */
#define DCPP_CMD_RANGE  0x06U   /* [sig] → [0|1] 窗口判决 */
#define DCPP_CMD_DATA   0x07U   /* [idx] → 组合帧 2B 小端 */
#define DCPP_CMD_OPEN   0x08U   /* 供电会话开启 → [1] */
#define DCPP_CMD_CLOSE  0x09U   /* 供电会话关断 → [0] */
#define DCPP_CMD_NUM    9U      /* 命令总数 */

/* 状态机 (逐字节 feed) */
typedef enum {
    HW_DCPP_IDLE = 0,   /* 等 START 首字节 */
    HW_DCPP_HDR,        /* 收 VER/SIDE/CMD/LEN */
    HW_DCPP_PAYLOAD,    /* 收负载 */
    HW_DCPP_CKSUM,      /* 收校验字节 */
    HW_DCPP_END         /* 收 END 标记 */
} hw_dcpp_state_t;

/* feed() 返回 */
#define HW_DCPP_FEED_MORE   0    /* 帧未收完, 继续喂 */
#define HW_DCPP_FEED_FRAME  1    /* 成帧并派发成功 */
#define HW_DCPP_ERR_FRAME  (-1)  /* 帧结构/起始定界错误 */
#define HW_DCPP_ERR_CKSUM  (-2)  /* 校验失败 */
#define HW_DCPP_ERR_END    (-3)  /* 结束定界错误 */
#define HW_DCPP_ERR_CMD    (-4)  /* 未知命令 */
#define HW_DCPP_ERR_LEN    (-5)  /* 负载超长 */
#define HW_DCPP_ERR_ARG    (-6)  /* 命令参数非法 */
#define HW_DCPP_ERR_CLOSED (-7)  /* 会话未开启 */

/* ---- 真机 BSP: 信号读取回调注入 (默认=模拟器) ----
 * 返回约定: >=0 = 信号读数; <0 = 不支持/读失败 (hw_dc 落 HW_DC_IOERR)。 */
typedef struct {
    int (*read)(int sig);        /* 读 DC 信号 0..7 原始值 */
    int (*base_read)(int side);  /* 读参考预值: 0=输入侧 1=输出侧 */
} hw_dc_bsp_t;

/* ============================================================
 * 用户原槽位 API (语义修复版: 判 NULL)
 * ============================================================ */
/* 装载 DC 信号表: args[0..7] = 8 信号取值 (NULL 则跳过装载) */
void dc_create_mode(int* args);
/* 极值窗口判决: 读 point[*offset] 是否落在 [MININ,MAXIN] 窗口内
 * 返回: 1 = 在窗口内 / 0 = 越界 / -1 = 参数非法 (point/offset 为 NULL) */
int  dc_ready(int* point, int* offset);
/* 导出: handler[0..7] <- 8 信号; point[0] <- 组合数据帧 DC_DATA_5 (均可为 NULL) */
void dc_init(int* handler, int* point);
/* 输入侧 8 信号快照指针 (指向模块内静态数组, 勿释放) */
int* dc_mode_input(void);

/* ============================================================
 * hw_dc 家族 API
 * ============================================================ */
uint8_t     hw_dc_mode(void);
const char* hw_dc_mode_str(uint8_t mode);

uint8_t     hw_dc_sig_count(void);                 /* = DC_SIG_COUNT (8) */
const char* hw_dc_sig_name(uint8_t sig);           /* INPUT/OUTPUT/... 或 "?" */
const char* hw_dc_sig_desc(uint8_t sig);           /* 中文描述 或 "?" */
const char* hw_dc_side_name(uint8_t side);         /* IN/OUT 或 "?" */

int         hw_dc_sig_value(uint8_t sig);          /* 当前信号值 (越界 -1) */
int         hw_dc_sig_set(uint8_t sig, int val);   /* 0 成功 / -1 越界 */
int         hw_dc_base_signal(uint8_t side);       /* 参考预值 (IN 0x059 / OUT 0x0080) */
int         hw_dc_base_read(uint8_t side);         /* 参考预值读取 (BSP 优先, 否则内置) */

uint8_t     hw_dc_data_count(void);                /* = DC_DATA_COUNT (7) */
uint32_t    hw_dc_data(uint8_t idx);               /* 第 idx 个组合帧 (越界 0) */

uint32_t    hw_dc_checksum(void);                  /* 信号表+数据表 FNV-1a-32 (黄金) */

void        hw_dc_init(void* arg);                 /* 上电: 黄金自证 + 信号/极值复位 */
void        hw_dc_hook(void* arg);                 /* 资源钩子 (当前无动态资源) */
void        hw_dc_bsp_install(const hw_dc_bsp_t* bsp);  /* 真机回调注入 (NULL=卸载回模拟) */

int         hw_dc_in_range(uint8_t sig);           /* 信号是否在 [MININ,MAXIN] / [MINOUT,MAXOUT] */

/* ---- 命令分发 (VM OP_HW_DC_CALL / CLI 共用) ----
 * 返回: >=0 结果 / -1 未识别或参数非法 / -2 help
 *   count        → 8 (信号数)         datacount → 7
 *   sig N        → 第 N 路信号当前值  name N → 信号名
 *   set N V      → 置信号值 → 0
 *   base N       → 参考预值 (0=IN/1=OUT)
 *   range N      → 1 在窗口内 / 0 越界
 *   data N       → 第 N 个组合帧
 *   ok           → 1 (黄金校验和匹配) / 0
 *   mode         → 模式编号
 *   protover     → 协议版本            protook → 1 (协议黄金匹配) / 0
 *   protobuild N → 用命令 N 打帧 → 帧长 (含 START/END 定界)
 *   protocmd N   → 喂一条命令 N 的完整帧 → 1 成帧 / 负错误码
 *   protoreset   → 复位协议会话 → 0   protostat → 状态机状态编号
 *   protovalue   → 上一条应答的 16 位值
 *   help         → -2
 */
int         hw_dc_cmd(const char* cmd, void* ctx);

/* ---- 自检 (putf=NULL 静默只返回失败数, 真机固件用) ---- */
int         hw_dc_selftest(int (*putf)(const char*));

/* ============================================================
 * DCPP 供电协议 API (帧层)
 * ============================================================ */

/* 协议黄金校验和: 命令表(编号+名) + 帧常量 FNV-1a-32 (独立 Python 锁定) */
uint32_t    hw_dc_proto_checksum(void);

uint8_t     hw_dc_proto_ver(void);           /* = HW_DCPP_VER (入帧/黄金) */
uint8_t     hw_dc_proto_cmd_num(void);       /* = DCPP_CMD_NUM (兼黄金) */
uint8_t     hw_dc_proto_state(void);         /* 当前状态机状态 hw_dcpp_state_t */
int         hw_dc_proto_opened(void);        /* 供电会话是否已开启 (OPEN..CLOSE) */
const char* hw_dc_proto_cmd_name(uint8_t cmd);  /* 命令名 PING..CLOSE 或 ? */
const char* hw_dc_proto_state_name(uint8_t st); /* IDLE/HDR/... 或 ? */
int         hw_dc_proto_frame_len(uint8_t len); /* 帧长 = 11+len; len 越界 <0 */

/* 打帧: 成帧到 out[], 返回帧长 (>0) 或 -1 (参数/容量错误) */
int         hw_dc_proto_build(uint8_t cmd, uint8_t side, const uint8_t* pl,
                              uint8_t len, uint8_t* out, int cap);
/* 单帧解析: 返回 >=0 应答字节数 / <0 错误码 (同 feed 的 HW_DCPP_ERR_*) */
int         hw_dc_proto_parse(const uint8_t* in, int len);

/* 逐字节喂入 (流式状态机): 返回 HW_DCPP_FEED_MORE / FEED_FRAME / 负错误码 */
int         hw_dc_proto_feed(uint8_t byte);
/* 取上一条成功帧的应答: 返回字节数 (0 = 无应答) */
int         hw_dc_proto_resp(uint8_t* out, int cap);
/* 取上一条成功帧的 16 位应答值 (GET/BASE/DATA 用), 无则 -1 */
int         hw_dc_proto_resp_value(void);
/* 复位状态机 + 会话 + 计数 (黄金/命令表不动) */
void        hw_dc_proto_reset(void);

/* 会话计数: rx(成帧数) / tx(应答数) / err(错误数) / last(最后成功命令号) */
void        hw_dc_proto_stats(uint32_t* rx, uint32_t* tx, uint32_t* err,
                              uint8_t* last);

/* ---- CLI: ./xiaomo dc [card|sig N|set N V|base N|range N|data N|mode|
 *                        selftest|proto ...] ---- */
int         hw_dc_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* HW_DC_H */
