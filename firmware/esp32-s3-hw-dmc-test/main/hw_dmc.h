#ifndef HW_DMC_H
#define HW_DMC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - DMC 主从链路协议层 (hw_dmc, 2026-10-01)
 *
 * 定位: DMC (Device Module Communication) 是一套 **主从 (master/slave)
 *       半双工字节协议**: 定长头 + 变长负载 + CRC16 校验 + 序号重传。
 *       典型场景: 主控 (ESP32) 与从设备模块 / 另一块 MCU 之间的可靠链路。
 *
 * 全跨式设计 (与 hw_token/hw_dc/hw_pin 家族同款):
 *   1. 编译期六模式探测 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *      构建系统可用 -DHW_DMC_MODE_OVERRIDE=n 强指。
 *   2. 核心 (成帧/解帧/CRC/握手/重传/状态机) 仅依赖 stdint + string,
 *      真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 底层收发走 BSP 回调注入:
 *        默认 = 环回 + 内置对端模型 (确定性, 全平台逐位一致可回归);
 *        真机固件 hw_dmc_bsp_install() 注入真实 UART, 协议逻辑零改动。
 *   4. 黄金参考 HW_DMC_GOLDEN = 0x169A603E
 *      (独立 Python 对拍锁定, 2026-10-01), selftest 逐项断言防未来误改。
 *
 * 六层接入: Makefile / OP_HW_DMC_CALL / mo2kbc 内置 / CLI dmc /
 *           examples/dmc_test.mo / tests/run_tests.sh
 *
 * ---------------------------------------------------------
 * ⚠️ 2026-10-01 三文件整合说明 (整合前**没有一份能编译**):
 *
 *  原 src/hw/ 下有三份互相冲突的 DMC 实现, 诊断结果:
 *
 *  ① include/hw_dmc.h  —— **伪代码, 恒不可编译**
 *     - `#error ""` 位于守卫内 = 任何编译器都直接失败
 *     - `crw(__std__)` / `cpt(__std__)`: `__std__` 全文未定义,
 *       且这是**解引用未定义标识符**, 连预处理都过不了
 *     - `#ifndef HW_DMC_START` 紧跟 `#define HW_DMC_START` = 永假分支
 *     - `#ifndef HW_DMC_END` 同样永假 → 该段永久死代码
 *     - `#if (crw(__std__)>>0x00891)` 拿位运算结果当条件 = 恒真/恒假不可控
 *     - 函数体内嵌 `#define` + 声明后无分号 + 赋值给宏常量 = 语法死结
 *     → **整份删除**, 不保留任何内容 (无任何可复用资产)。
 *
 *  ② src/hw/hw_dmc_porto.c —— 面向 STC12C5A60S2 (8051) 的串口实现
 *     - `SBUF` / `TI` / `RI` 三个 SFR 全文未定义 (8051 专用, 本项目无 8051 头)
 *     - `#include "dmc_proto.h"` —— **该头文件不存在** (实际叫 hw_dmc_porto.h)
 *     - `memcpy(&frm.payload[2], data, data_len)` 未查 data_len 上界,
 *       而 `payload[248]` → 调用方给 >246 即**栈溢出**
 *     - `dmc_recv_frame` 的 `frame->len - 4` 是 uint8_t 运算,
 *       len < 4 时回绕成 250+ → **栈上数组越界写**
 *     - 握手失败 `return -1` 与 CRC 失败 `return -1` **同码**, 调用方无法区分
 *     - `dmc_handshake_slave` 是 `while(1)` 无超时 = 永久挂死
 *     → **算法资产被完整吸收** (帧格式/命令号/CRC 语义), 实现全部重写。
 *
 *  ③ src/hw/hw_dmc_handshake.c —— 唯一逻辑完整的一份
 *     - 状态机/成帧/解帧/序号/ACK 重传 都写对了, **作为整合基线保留**
 *     - 但 CRC 用 `0x1021` (MSB-first), 而 porto 用 `0x8408` (反射式)
 *       → 两份**互相不兼容**, 同一份数据双端算出不同 CRC
 *     - `crw(0x40000000)` 裸内存映射 → 宿主上读非法地址 = SIGSEGV
 *     - `dmc_send_data` 的 `uint8_t payload[252]` = 252B 局部数组,
 *       调用链叠加 > 真机任务栈 (见 SKILL 坑 #12)
 *     - `dmc_recv_raw` 用空转 for 循环当 tick → 超时行为不确定, 不可回归
 *     - `dmc_dump_stats` 调 `crw_dump()` —— 该符号随伪代码头一起消失
 *     → **保留协议语义, 修掉 4 个真 bug** (见下)。
 *
 *  ④ 整合后修掉的 4 个真 bug (原三份文件全部存在):
 *     - **CRC 算法统一**: 全家族统一 CCITT-FALSE (0x1021 / init 0xFFFF /
 *       MSB-first / 不反射 / 不异或输出), 对国际标准向量
 *       `"123456789" -> 0x29B1`; 原 porto 的 0x8408 反射式作废。
 *     - **`dmc_unpack_frame` 越界**: 原码用 `declared_len - 2 - 2` 算 CRC 长度,
 *       而 declared_len 是 uint8_t (上限 255), `in[declared_len-1]` 在
 *       `declared_len > in_len` 时先于长度校验读取 → 本版**先校验后索引**。
 *     - **负载上界**: `dmc_send_data` 原码 `memcpy(&payload[2], data, len)`
 *       查了 `len > MAX-2` 但 **payload 是 252B 局部数组**;
 *       本版上界检查在 pack 前, 且大数据走 BSP 直发不落大缓冲。
 *     - **超时可回归**: 原 `for(volatile d=0;d<100;d++){}` 空转当 tick,
 *       编译优化下行为不确定; 本版超时改由 BSP 的 `now_ms()` 提供,
 *       默认实现 = 确定性 tick 计数器 (全平台逐位一致)。
 * ============================================================ */

/* ---- 编译期模式 (全跨式家族统一: 0=HOST .. 5=TEST) ---- */
typedef enum {
    HW_DMC_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_DMC_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux) */
    HW_DMC_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_DMC_MODE_ESP32   = 3,   /* ESP32 / S3 / C3 / C6 (IDF) */
    HW_DMC_MODE_ESP8266 = 4,   /* ESP8266 */
    HW_DMC_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_DMC_MODE_MAX     = 6
} hw_dmc_mode_t;

/* ---- 当前运行模式 (编译期宏优先, 运行期缓存一次) ---- */
int hw_dmc_mode(void);
const char* hw_dmc_mode_name(int mode);
const char* hw_dmc_build_arch(void);

/* ---- 协议常量 ---- */
#define HW_DMC_SYNC_0        0xAAU
#define HW_DMC_SYNC_1        0x55U
#define HW_DMC_HEADER_LEN    4U    /* sync0 + sync1 + len + cmd */
#define HW_DMC_CRC_LEN       2U
/* ⚠️ LEN 字段是 **1 字节**（覆盖 sync0..crc 末 = 整帧长度）→ 整帧最长 255。
 *    载荷上限 = 255 − 4(头) − 2(CRC) = 249。
 * 🕳️ 历史缺陷（2026-10-01 宿主镜像抓获，源自归档原件 handshake.c）：
 *    初版 MAX_PAYLOAD=252 → MAX_FRAME=258，而 258 **装不进 1 字节**：
 *    pack 的 `out[2]=(uint8_t)258` 静默回绕成 2 → 接收端 declared=2 <
 *    MIN_FRAME(6) 直接判 FRAME；真链路上更会按 2 字节切帧 → 整条流错位。
 *    载荷 ∈ {250,251,252} 的帧**结构性无法解出**，而 selftest [13] 当时只
 *    断言 pack 的返回长度、从不 round-trip → 缺陷被完整遮住。
 *    修法 = 把上限改成「一个字节装得下」的 249（整帧 255）。 */
#define HW_DMC_MAX_PAYLOAD   249U
#define HW_DMC_MIN_FRAME     (HW_DMC_HEADER_LEN + HW_DMC_CRC_LEN)              /* 6 */
#define HW_DMC_MAX_FRAME     (HW_DMC_HEADER_LEN + HW_DMC_MAX_PAYLOAD + HW_DMC_CRC_LEN) /* 255 */

/* ---- 草稿常量集 (源自 src/hw/hw_dmc_base.c 伪代码底稿, 2026-10-01 提取) ----
 *
 * 【为什么现在才有】伪代码底稿 100 行整体永远编不了 (C 编译器在第 60-63 行报
 * 3 个硬错误: `extern "C"{` / `#error ""` / `#ifdef **std**` 非法宏名), 但
 * 散落在废码里的这 8 个常量是**真实的设计意图**, 且此前全模块一个都没被吸收。
 * 归档件整份编不了 ≠ 这批常量该跟着一起烂掉 —— 故提取为真定义。
 *
 * 🕳️ 自查教训: 初查时用 `grep -c "define X"` 得到"7 缺 1 有"是**错的** ——
 *    那第 1 个"有"命中的是第 38 行**描述缺陷的注释文字**, 不是真定义。
 *    字面量匹配会把注释当实现, 须排注释行(`^[[:space:]]*#define`)再数。
 *
 * 语义 (按草稿内出现的上下文推断, 草稿本身未给说明):
 *   HW_DMC_START 帧起始定界标记
 *   HW_DMC_END   帧结束定界标记
 *   HW_DMC_TMP / HW_DMC_DUMP   临时区 / 导出区地址 (草稿中均为 0x00, 相对寻址基址)
 *   HW_DMC_UDP   0x90  上行通道偏移
 *   HW_DMC_DDP   0x28  下行通道偏移
 *   HW_DMC_EMU   0x68  仿真器映射偏移
 *   HW_DMC_MCU   0x100010  MCU 区映射偏移
 * ⚠️ 这些是**区段偏移**, 与本协议的 LEN/CRC 计算无关, 不参与成帧。
 *    保留它们是为了让草稿的可追溯信息留在活代码里, 而不是只躺在归档注释中。
 */
#define HW_DMC_TMP    0x00U
#define HW_DMC_DUMP   0x00U
#define HW_DMC_UDP    0x90U
#define HW_DMC_DDP    0x28U
#define HW_DMC_EMU    0x68U
#define HW_DMC_MCU    0x0010010U
#define HW_DMC_START  0x01U
#define HW_DMC_END    0x19U
/* 草稿声明的区段总数 (DMC_DATA_COUNT) */
#define HW_DMC_DATA_COUNT 7U

/* ---- 9 条命令 (与 Python 黄金表同源) ---- */
typedef enum {
    HW_DMC_CMD_HELLO       = 0x01,
    HW_DMC_CMD_HELLO_ACK   = 0x02,
    HW_DMC_CMD_DATA        = 0x03,
    HW_DMC_CMD_DATA_ACK    = 0x04,
    HW_DMC_CMD_RESET       = 0x05,
    HW_DMC_CMD_RESET_ACK   = 0x06,
    HW_DMC_CMD_STATUS      = 0x07,
    HW_DMC_CMD_STATUS_RSP  = 0x08,
    HW_DMC_CMD_NACK        = 0x0F
} hw_dmc_cmd_t;
#define HW_DMC_CMD_NUM 9

/* ---- 7 个状态 (与 Python 黄金表同源) ---- */
typedef enum {
    HW_DMC_ST_IDLE          = 0,
    HW_DMC_ST_SEND_HELLO    = 1,
    HW_DMC_ST_WAIT_HELLO_ACK= 2,
    HW_DMC_ST_SEND_RESET    = 3,
    HW_DMC_ST_WAIT_RESET_ACK= 4,
    HW_DMC_ST_ESTABLISHED   = 5,
    HW_DMC_ST_ERROR         = 6
} hw_dmc_state_t;
#define HW_DMC_STATE_NUM 7

/* ---- 错误码 (与原 handshake 一致, 保留 7 个) ---- */
typedef enum {
    HW_DMC_ERR_OK      = 0,
    HW_DMC_ERR_TIMEOUT = 1,
    HW_DMC_ERR_CRC     = 2,
    HW_DMC_ERR_FRAME   = 3,
    HW_DMC_ERR_STATE   = 4,
    HW_DMC_ERR_NACK    = 5,
    HW_DMC_ERR_BUSY    = 6,
    HW_DMC_ERR_PARAM   = 7    /* 整合新增: 参数/上界越界 (原三份都没有) */
} hw_dmc_err_t;
#define HW_DMC_ERR_NUM 8

const char* hw_dmc_cmd_name(uint8_t cmd);
const char* hw_dmc_state_name(int st);
const char* hw_dmc_err_name(int e);

/* ============================================================
 * BSP: 底层收发注入
 * ============================================================ */
typedef struct {
    /* 发送一字节; 返回 >=0 成功, <0 失败 */
    int  (*tx)(uint8_t ch);
    /* 收一字节, 超时 timeout_ms 未到返回负值; 返回 >=0 收到字节, <0 超时/错 */
    int  (*rx)(uint32_t timeout_ms);
    /* 毫秒时基 (真机 = esp_timer / gettimeofday; 默认 = 确定性 tick 计数器) */
    uint32_t (*now_ms)(void);
    /* 硬件收是否有待处理字节 (0/1); NULL = 走 rx() 的阻塞语义 */
    int  (*rx_ready)(void);
} hw_dmc_bsp_t;

/* ============================================================
 * 链路实例 (不透明, 调用方按值持有)
 * ============================================================ */
typedef struct {
    int      state;             /* hw_dmc_state_t */
    uint8_t  local_addr;
    uint8_t  slave_addr;
    uint16_t seq_tx;
    uint16_t seq_rx;
    uint8_t  retries;
    uint32_t timeout_ms;
    /* stats */
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t err_crc;
    uint32_t err_timeout;
    uint32_t err_frame;
    /* 最近一次收到/发出的命令 (诊断用) */
    uint8_t  last_rx_cmd;
    uint8_t  last_tx_cmd;
} hw_dmc_link_t;

/* ---- 生命周期 ---- */
void hw_dmc_init(void* arg);                     /* 上电复位 (⚠️ 不清 BSP, 见下) */
void hw_dmc_hook(void* arg);
void hw_dmc_bsp_install(const hw_dmc_bsp_t* bsp); /* NULL = 卸载回默认环回 */

/* ---- 协议原语 (可单测, 不依赖链路状态) ---- */
uint16_t hw_dmc_crc16(const uint8_t* data, uint16_t len);
uint32_t hw_dmc_fnv1a(const uint8_t* p, uint32_t n);
int      hw_dmc_pack(uint8_t cmd, const uint8_t* payload, uint16_t plen,
                     uint8_t* out, int cap);
int      hw_dmc_unpack(const uint8_t* in, int len, uint8_t* cmd_out,
                       const uint8_t** pl_out, uint16_t* plen_out);

/* ---- 链路操作 ---- */
void hw_dmc_link_init(hw_dmc_link_t* l, uint8_t local_addr, uint8_t slave_addr,
                      uint32_t timeout_ms);
int  hw_dmc_link_state(const hw_dmc_link_t* l);
int  hw_dmc_handshake_master(hw_dmc_link_t* l, int max_retries);
int  hw_dmc_handshake_slave(hw_dmc_link_t* l, uint32_t wait_timeout_ms);
int  hw_dmc_send_data(hw_dmc_link_t* l, const uint8_t* data, uint16_t len,
                      int max_retries);
int  hw_dmc_recv_data(hw_dmc_link_t* l, uint8_t* out, int cap, uint16_t* recv_len,
                      uint32_t timeout_ms);
int  hw_dmc_reset_link(hw_dmc_link_t* l, int max_retries);
int  hw_dmc_query_status(hw_dmc_link_t* l, uint8_t* status_byte, uint32_t timeout_ms);
void hw_dmc_stats(const hw_dmc_link_t* l, uint32_t* tx, uint32_t* rx,
                  uint32_t* crc_err, uint32_t* to_err);

/* ---- 草稿 API 意图层 (2026-10-01 提取并实现) --------------------------------
 *
 * 【来源】src/hw/hw_dmc_base.c 伪代码底稿第 107-114 行, 原文 4 行声明:
 *     int dmc_proto_run_cmd(uint8_t cmd);
 *     int dmc_porto_init(uint8_t cmd);
 *     int dmc_proto_create(uint8_t cmd);
 *     int dmc_proto_ready(uint8_t cmd);
 *
 * 【为什么不照抄签名】四者签名同为 `int f(uint8_t cmd)`, 但 create/ready/porto_init
 * 语义上**不该**吃一个 cmd 参数 —— 那是草稿的未完成痕迹(函数体整体缺失, 前后
 * 夹着对宏赋值与 `#error`)。照抄等于把无意义的参数固化进 API。故按各自动词的真实
 * 语义给出可用签名, 名字保持一一对应, 便于回溯。
 *
 * 【这一层存在的意义】草稿表达的是"协议对象生命周期"这套结构:
 *     porto_init (装底层) → proto_create (建对象) → proto_ready (就绪判定)
 *     → run_cmd (发命令)
 * 此前 hw_dmc.c 只有一堆平铺的函数, 缺这层组织。这里补上, 全部**薄封装既有
 * 原语**, 不复制任何协议逻辑, 故不引入第二份实现。
 */
int hw_dmc_porto_init(const hw_dmc_bsp_t* bsp);   /* 草稿 dmc_porto_init  */
int hw_dmc_proto_create(hw_dmc_link_t* l, uint8_t local_addr,
                        uint8_t slave_addr, uint32_t timeout_ms); /* proto_create */
int hw_dmc_proto_ready(const hw_dmc_link_t* l);  /* 草稿 dmc_proto_ready */
int hw_dmc_proto_run_cmd(hw_dmc_link_t* l, uint8_t cmd,
                         const uint8_t* payload, uint16_t plen,
                         uint8_t* resp, int resp_cap, int* resp_len,
                         uint32_t timeout_ms);
                                                /* 草稿 dmc_proto_run_cmd
                                                 * ⚠️ 响应长度走 out-param 而非
                                                 *    返回值, 理由见下方实现注释 */

/* ---- 黄金值 (独立 Python 对拍锁定) ---- */
#define HW_DMC_GOLDEN        0x169A603EU
#define HW_DMC_CRC_VECTOR    0x29B1U   /* CRC16-CCITT("123456789") 国际标准向量 */
#define HW_DMC_CMD_FNV       0x80EEF1A6U
#define HW_DMC_STATE_FNV     0x9B71FE5FU
#define HW_DMC_FRAME_FNV     0xB3DFA0D8U

/* ---- 辅助 (CLI / 示例 / selftest 用) ---- */
void hw_dmc_loop_reset(void);              /* 清默认环回缓冲 + tick + tx 标记 */
int  hw_dmc_loop_inject(const uint8_t* data, int len); /* 注入「对端」字节 (不打 tx 标) */
int  hw_dmc_loop_txmark(void);             /* 本端已发送字节数 (断言帧真发出) */
int  hw_dmc_crc_vector(uint8_t* out, int cap);   /* 取 "123456789" 9 字节 */
uint16_t hw_dmc_crc_of_vector(void);       /* = 0x29B1 (国际标准向量) */

/* ---- 命令分发 (VM opcode / CLI 共用) ----
 * 单参: count|cmds|states|errs|maxpay|maxframe|minframe|mode|frame|crcvec|
 *       hello|golden|cmdfnv|statefnv|framefnv|ok|crclen|help
 *       (help 返回 -2, 未识别返回 -1)
 * ⚠️ 数值参数**不经过本函数**: mo2kbc 双参内置 hw_dmc("fmt", 数值) 编译成
 *    OP_HW_DMC_CALL 的 imm=R_TMP+1, VM 侧把寄存器值十进制**拼到命令串尾**
 *    再调进来 (见 vm_core.c case OP_HW_DMC_CALL)。
 *    本函数内部不含 atoi/strncmp —— 这是**有意的**:
 *    VM 侧已经拼好字符串, 这里再解析一次就是第二个真相源。
 *    例: void v:int=7;  hw_dmc("frame ", v) → 本函数收到 "frame 7"
 * ⚠️ 旧版本此处写着"双参: frame <cmd> <plen> | state <n>" —— 从未实现,
 *    照着写 .mo 只会拿到 -1。已删除, 避免再次把注释当契约。 */
int hw_dmc_cmd(const char* cmd, void* ctx);

/* ---- selftest (putf=NULL 时静默只返回结果码) ---- */
int hw_dmc_selftest(int (*putf)(const char*));

/* ---- CLI (家族约定: 读 argv[2], 判 argc >= 3) ---- */
int hw_dmc_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* HW_DMC_H */
