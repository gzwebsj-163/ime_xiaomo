#ifndef HW_FLASH_H
#define HW_FLASH_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - ESP32 ROM 下载协议烧录层 (hw_flash, 2026-09-30)
 *
 * 职责: 把「给 ESP32 烧固件」这件事从外部工具 (esptool/Python)
 *       收进 xiaomo 架构本身 —— 纯 C、全跨式六模式:
 *       HOST / LINUX / KELL / ESP32 / ESP8266 / TEST。
 *       自此烧录可由 .mo 字节码驱动 (hw_flash("run 4096"))。
 *
 * 协议面 (ESP32 ROM serial bootloader, 与 esptool loader.py 同源):
 *   1. SLIP (RFC1055) 帧封装 : 0xC0 定界 + 0xDB 转义
 *        字面 C0 -> DB DC ; 字面 DB -> DB DD   (方向不可反!)
 *   2. 请求帧  "<BBHI" : dir(1)+cmd(1)+len(2)+checksum(4) + body
 *        checksum = body 逐字节异或, 初值 ESP_CHECKSUM_MAGIC(0xEF)
 *   3. 应答帧  "01 op len(u16) val(u32)" + data[len] + status(1) + error(1)
 *        ⚠️ 应答头第 3 字段 = **data 长度**(不是请求回显), 第 4 字段 = val;
 *        末两字节 = [status, error], 缺了即 "Invalid response"
 *        (与已验证的 IDE 模拟器 pty_sim.c rom_reply() 逐字对齐)
 *   4. ROM 命令集 (S3 ROM 无 stub 全放行):
 *        0x02 FLASH_BEGIN   0x03 FLASH_DATA   0x04 FLASH_END
 *        0x08 SYNC          0x09 WRITE_REG   0x0A READ_REG
 *        0x0F CHANGE_BAUD   0x13 FLASH_MD5   0x14 GET_SECURITY_INFO
 *   5. 无 stub 时块尺寸 = 1024B ; ROM 模式不收 FLASH_END,
 *      最后一条协议命令是 SPI_FLASH_MD5(0x13), 由它触发烧完校验。
 *
 * 全跨式设计 (与 hw_token / hw_fault / hw_wdbg 同款):
 *   1. 编译期模式探测六模式, -DHW_FLASH_MODE_OVERRIDE=n 可强指。
 *   2. 核心 (SLIP/帧/XOR 校验/MD5/烧录流程) 只依赖 stdint + string,
 *      真机 (ESP32/ESP8266/内核嵌入) 可 freestanding 编译。
 *   3. 底层字节收发走 BSP 回调: 默认 = 确定性 ROM 模拟器
 *      (表驱动, S3 ROM 语义, 全平台逐位一致可回归);
 *      真机固件 hw_flash_bsp_install() 注入真实 UART, 上层零改动。
 *   4. 黄金参考: HW_FLASH_GOLDEN = ROM 命令表 FNV-1a-32 (独立 Python
 *      对拍锁定, 2026-09-30), selftest 逐项断言防未来误改。
 *
 * 六层接入 (2026-09-30):
 *   Makefile   HW_SRCS 收编 (Makefile.linux 同补)
 *   VM 内核    OP_HW_FLASH_CALL (vm_core.c, kvm_run 上电自动 hw_flash_init)
 *   编译器     mo2kbc 内置 hw_flash("...") → OP_HW_FLASH_CALL
 *   CLI        ./xiaomo flash [card|mode|slip|md5|sync|chip|run N|verify|selftest]
 *   示例       examples/flash_test.mo (.kbc 端到端)
 *   测试       tests/run_tests.sh flash 块
 * ============================================================ */

/* ---- 编译期模式 (与 hw_token 同款) ---- */
typedef enum {
    HW_FLASH_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_FLASH_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux) */
    HW_FLASH_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_FLASH_MODE_ESP32   = 3,   /* ESP32-S3/C3/C6 (IDF) */
    HW_FLASH_MODE_ESP8266 = 4,   /* ESP8266 (RTOS/NonOS) */
    HW_FLASH_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_FLASH_MODE_MAX     = 6
} hw_flash_mode_t;

/* ---- ROM 命令码 (esptool loader.py, 勿改) ---- */
#define HW_FLASH_CMD_FLASH_BEGIN   0x02u
#define HW_FLASH_CMD_FLASH_DATA    0x03u
#define HW_FLASH_CMD_FLASH_END     0x04u
#define HW_FLASH_CMD_SYNC          0x08u
#define HW_FLASH_CMD_WRITE_REG     0x09u
#define HW_FLASH_CMD_READ_REG      0x0Au
#define HW_FLASH_CMD_CHANGE_BAUD   0x0Fu
#define HW_FLASH_CMD_FLASH_MD5     0x13u   /* SPI_FLASH_MD5 */
#define HW_FLASH_CMD_SEC_INFO      0x14u   /* GET_SECURITY_INFO */

/* ---- 帧方向 ---- */
#define HW_FLASH_DIR_REQ    0x00u
#define HW_FLASH_DIR_RSP    0x01u

/* ---- 应答状态尾 ---- */
#define HW_FLASH_ST_OK      0x00u
#define HW_FLASH_ST_FAIL    0x01u

/* ---- SLIP (RFC1055) ---- */
#define HW_FLASH_SLIP_END     0xC0u   /* 帧定界 */
#define HW_FLASH_SLIP_ESC     0xDBu   /* 转义引导 */
#define HW_FLASH_SLIP_ESC_END 0xDCu   /* 字面 C0 的转义形态: DB DC */
#define HW_FLASH_SLIP_ESC_ESC 0xDDu   /* 字面 DB 的转义形态: DB DD */

/* ---- 校验和魔数 (ESP_CHECKSUM_MAGIC) ---- */
#define HW_FLASH_CHECKSUM_MAGIC 0xEFu

/* ---- 容量 ---- */
#define HW_FLASH_BLOCK_MAX   1024u    /* S3 ROM 无 stub 块尺寸 */
#define HW_FLASH_HDR_LEN     8u       /* 请求/应答帧头 */
#define HW_FLASH_DATA_HDR    16u      /* FLASH_DATA 请求体前缀: size+seq+2×0 */
#define HW_FLASH_RSP_TAIL    2u       /* status + error */
/* SLIP 编码最坏膨胀: 单帧 (8+16+1024+2) -> 2x + 2 定界 */
#define HW_FLASH_SLIP_CAP    ((HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX + 4u) * 2u + 2u)

/* ROM 模拟器 flash 模型容量 (真机烧录不占此空间; -DHW_FLASH_MODEL_CAP 可调) */
#ifndef HW_FLASH_MODEL_CAP
#define HW_FLASH_MODEL_CAP   32768u
#endif

/* ---- 黄金值 (独立 Python 对拍锁定 2026-09-30, 勿改) ---- */
#define HW_FLASH_GOLDEN        0xAF05978Au  /* ROM 命令表 FNV-1a-32 */
/* 4096B 确定性测试镜像的 MD5 (Python hashlib 对拍锁定, 勿改) */
#define HW_FLASH_IMG4096_MD5   "4e328028738d17bb7ff82667d5803369"
#define HW_FLASH_CHIP_ID       0x00000009u  /* ESP32S3 IMAGE_CHIP_ID */
#define HW_FLASH_RDID_WINBOND  0x001640EFu  /* 4MB Winbond RDID(0x9F) 回读 */
#define HW_FLASH_SPI_MAGIC_ADDR  0x60001F10u
#define HW_FLASH_SPI_CMD_ADDR    0x60002000u
#define HW_FLASH_SPI_RDID_ADDR   0x60002058u

/* ---- 返回码 ---- */
#define HW_FLASH_R_OK        0x00
#define HW_FLASH_R_NOARGS    0x01
#define HW_FLASH_R_BADARG    0x02
#define HW_FLASH_R_IOERR     0x03
#define HW_FLASH_R_PROTO     0x04   /* 协议错 (SLIP/帧/状态尾不符) */
#define HW_FLASH_R_CHECKSUM  0x05   /* XOR 校验和不符 */
#define HW_FLASH_R_BADSIZE   0x06   /* 镜像尺寸非法 (0 或超模型容量) */
#define HW_FLASH_R_NOCMD     (-1)   /* 未识别命令 */
#define HW_FLASH_R_HELP      (-2)   /* help 已吐出 */

/* ---- 烧录会话 ---- */
typedef struct {
    uint32_t offset;          /* flash 内起始偏移 */
    uint32_t image_size;      /* 镜像总字节 */
    uint32_t block_size;      /* 本次会话块尺寸 (<= HW_FLASH_BLOCK_MAX) */
    uint32_t blocks_written;  /* 已写块数 */
    uint32_t bytes_written;   /* 已写字节 */
    uint32_t progress;        /* 0..100 */
    uint8_t  done;            /* 1 = 流程走完 */
    uint8_t  md5_match;       /* 1 = 设备回读 MD5 == 本地 MD5 */
    uint8_t  md5_local[16];   /* 本地镜像 MD5 */
    uint8_t  md5_remote[16];  /* 设备回读 MD5 */
} hw_flash_session_t;

/* ---- 状态快照 (跨会话累计) ---- */
typedef struct {
    uint32_t sessions;        /* 烧录会话数 */
    uint32_t cmd_tx;          /* 发出的 ROM 命令数 */
    uint32_t cmd_rx;          /* 收到的应答数 */
    uint32_t slip_escaped;    /* 编码时转义字节数 */
    uint32_t bytes_flashed;   /* 累计写入 flash 字节 */
    uint32_t last_md5_ok;     /* 最近一次 MD5 是否一致 */
    /* 🆕 stale_frames (2026-10-02, A-4②): 被判为「不是本命令应答」而丢弃的帧数。
     *   真机 SYNC 回 8 帧 ⇒ 正常应出现 7。**这个计数是让修复可观测的关键** ——
     *   没有它, 「残留被丢弃」和「残留被误当应答」在日志上长得一模一样。
     *   真机若此值持续为 0 而 SYNC 后 chip_id 又不对, 说明固件只回了 1 帧。 */
    uint32_t stale_frames;
    uint8_t  chip_id;         /* 最近一次探测的芯片 ID */
} hw_flash_stat_t;

/* ---- 真机 BSP: 底层字节收发回调注入 (默认 = 确定性 ROM 模拟器) ----
 * 返回约定: uart_open/close 0=成功 (open 失败回 <0);
 * uart_write 返回实际写入字节 (<0 失败);
 * uart_read 返回读到字节 (0=暂无, <0 失败)。 */
typedef struct {
    int (*uart_open)(uint32_t baud);
    int (*uart_close)(void);
    int (*uart_write)(const uint8_t* d, uint32_t n);
    int (*uart_read)(uint8_t* d, uint32_t cap);
} hw_flash_bsp_t;

/* ============================================================
 * API
 * ============================================================ */

uint8_t     hw_flash_mode(void);
const char* hw_flash_mode_str(uint8_t mode);
const char* hw_flash_result_code_str(int rc);

/* ---- SLIP 编解码 (RFC1055; 返回输出字节数, <0 失败) ---- */
int  hw_flash_slip_encode(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap);
int  hw_flash_slip_decode(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap);
/* ⚠️ 多帧消费版 (2026-10-02, A-4② 配套): 上面那个返回的是**解码后**长度,
 *   不是**消费的输入**长度 —— 无转义时两者差 2(前后定界符), **有转义时不等**。
 *   要丢弃「已读走的一帧、留下残余」就必须用这个。
 *   *consumed 仅在**确实解到收尾定界**(帧收全)时才写, 否则置 0 ⇒ 上层继续等。
 *   单帧场景用 hw_flash_slip_decode 即可, 行为与旧版逐位一致。 */
int  hw_flash_slip_decode_ex(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap,
                             uint32_t* consumed);

/* ---- 请求帧: "<BBHI" dir+cmd+len+xor_checksum + body ---- */
int  hw_flash_frame_build(uint8_t cmd, const uint8_t* body, uint32_t blen,
                          uint8_t* out, uint32_t cap);
/* 解析应答帧: "01 op len(u16) val(u32)" + data[len] + status + error
 * val 取自帧头; body 指针指向帧内 data, 不拷贝 */
int  hw_flash_frame_parse(const uint8_t* in, uint32_t n, uint8_t* cmd, uint32_t* val,
                          const uint8_t** body, uint32_t* blen, uint8_t* st);

/* ---- XOR 校验和 (初值 0xEF) ---- */
uint8_t     hw_flash_checksum(const uint8_t* d, uint32_t n);

/* ---- MD5 (RFC1321, 纯整数实现) ---- */
void        hw_flash_md5(const uint8_t* d, uint32_t n, uint8_t out16[16]);
void        hw_flash_md5_hex(const uint8_t* d, uint32_t n, char out33[33]);

/* ---- 确定性测试镜像 (含 C0/DB 字节, 用于 SLIP 转义回归) ---- */
uint32_t    hw_flash_test_image(uint8_t* buf, uint32_t n);

/* ---- ROM 命令表 (只读) 与黄金校验和 ---- */
typedef struct { uint8_t code; const char* name; } hw_flash_cmd_info_t;
const hw_flash_cmd_info_t* hw_flash_cmd_table(void);
uint32_t    hw_flash_cmd_count(void);
uint32_t    hw_flash_cmd_checksum(void);   /* FNV-1a-32, 应 == HW_FLASH_GOLDEN */

/* ---- 生命周期 (kvm_run 上电自动调 init) ---- */
void        hw_flash_init(void* arg);       /* 复位会话/计数器/BSP/模型 */
void        hw_flash_hook(void* arg);       /* 资源钩子 (当前无动态资源) */
void        hw_flash_bsp_install(const hw_flash_bsp_t* bsp);  /* NULL=还原默认模拟器 */

/* ---- 协议原语 ---- */
int  hw_flash_sync(void);                             /* SYNC 握手 */
int  hw_flash_read_reg(uint32_t addr, uint32_t* val); /* READ_REG */
uint32_t hw_flash_chip_id(void);                      /* 探测芯片 ID */
void hw_flash_stat(hw_flash_stat_t* out);
void hw_flash_result(char* buf, uint32_t cap);

/* ---- 烧录流程 (镜像 -> 设备, 内部走 BSP) ----
 * 成功返回 HW_FLASH_R_OK 且 session->md5_match==1。
 * flush=1 时每块调 progress_cb (可为 NULL) 回报百分比。 */
typedef void (*hw_flash_progress_fn)(uint32_t pct, uint32_t bytes, void* ctx);
int  hw_flash_run(const uint8_t* image, uint32_t len, uint32_t offset,
                  hw_flash_session_t* out, hw_flash_progress_fn progress_cb, void* ctx);

/* ---- 命令分发 (VM OP_HW_FLASH_CALL 用); >=0 结果码 / -1 未识别 / -2 help ---- */
int  hw_flash_cmd(const char* cmd, void* ctx);

/* ---- 自检 (putf=NULL 静默) → 失败项数 (0 = 全过) ---- */
int  hw_flash_selftest(int (*putf)(const char*));
/* CLI: ./xiaomo flash [...] */
int  hw_flash_cli(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* HW_FLASH_H */
