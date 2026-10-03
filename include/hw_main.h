/*
 * xiaomo - hw 家族统一类注册表 + 总调度 (hw_main, 2026-09-28)
 *
 * 用户草稿定版 (2026-09-28, 类 ID 勿改):
 *   8 大 hw 模块各挂一个类 ID, hw_main 做家族统一入口:
 *   统一类表 / 统一健康探针 / 统一 selftest / 统一 VM opcode。
 *
 * 全跨式整合纪律 (同 hw_token/hw_fault/hw_core):
 *   auto 返回类型 (C 文件作用域非法) → 改 const int*;
 *   dev_class 重复声明 → 去重; hw_class 枚举体补全。
 */
#ifndef HW_MAIN_H
#define HW_MAIN_H

#include <stdint.h>

/* ---- 用户定版类 ID (2026-09-28, 勿改) ---- */
#define ASR_CLASS    0x0058U
#define CORE_CLASS   0x0098U
#define DEV_CLASS    0x0089U
#define DIRECT_CLASS 0x01EFU
#define FAULT_CLASS  0x07DEU
#define HEX_CLASS    0x00DEFU
#define OEM_CLASS    0x00FFDU
#define TOKEN_CLASS  0x00EFDU
/* USB-C PD / 快充采集层。0x5044 = ASCII 'P''D' 助记, 与已用 8 个类 ID 无冲突
 * (已用: 0x0058/0x0098/0x0089/0x01EF/0x07DE/0x0DEF/0x0FFD/0x0EFD)。
 * 🕳️ 用户定版, 勿改 —— 改了 hw_main_checksum() 黄金必变。 */
#define USBPD_CLASS  0x5044U

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 类表序号 (hw_class, 表序 = 类名字母序, USBPD 排在 TOKEN 之后) ---- */
typedef enum {
    HW_CLASS_ASR    = 0,   /* hw_asr    双引擎语音识别 */
    HW_CLASS_CORE   = 1,   /* hw_core   内核 DNA 编码层 */
    HW_CLASS_DEV    = 2,   /* hw_dev    设备命令分发 */
    HW_CLASS_DIRECT = 3,   /* hw_direct 底层硬件直访 */
    HW_CLASS_FAULT  = 4,   /* hw_fault  故障诊断 */
    HW_CLASS_HEX    = 5,   /* hw_hex    十六进制工具 */
    HW_CLASS_OEM    = 6,   /* hw_oem    熔丝签名层 */
    HW_CLASS_TOKEN  = 7,   /* hw_token  跨模式令牌层 */
    HW_CLASS_USBPD  = 8,   /* hw_usbpd  USB-C PD/快充判读 (2026-10-03 登记) */
    HW_MAIN_CLASS_MAX = 9
} hw_class;

/* ---- 六模式 (家族同款编号) ---- */
#define HW_MAIN_MODE_HOST    0   /* 宿主 (macOS/Windows) */
#define HW_MAIN_MODE_LINUX   1   /* Linux (kickpi/server/Termux) */
#define HW_MAIN_MODE_KELL    2   /* 内核嵌入 (TinyEMU riscv64 真内核) */
#define HW_MAIN_MODE_ESP32   3   /* ESP32-S3/C3/C6 (IDF) */
#define HW_MAIN_MODE_ESP8266 4   /* ESP8266 (RTOS/NonOS) */
#define HW_MAIN_MODE_TEST    5   /* 测试 */

/* ---- 探针结果码 ---- */
#define HW_MAIN_RC_OK       0    /* 健康 */
#define HW_MAIN_RC_BAD     (-1)  /* 探针执行, 结果异常 */
#define HW_MAIN_RC_UNPROBED (-2) /* 尚未探过 */
#define HW_MAIN_RC_NOLINK  (-3)  /* 模块未链接 (真机固件按需裁剪) */

/* ---- 用户草稿函数定版 (auto → const int*, 语义补全) ----
 * 返回 const int[4] = {cls, idx, mode, probe}:
 *   [0]=类ID [1]=表序 [2]=六模式 [3]=最近探针结果
 * args==NULL  → 只读上次探针结果 (无副作用);
 * args[0]==1  → 现场重探一次后返回 (诊断模式)。
 * 未链接模块 → [3]=HW_MAIN_RC_NOLINK, 其余字段仍有效。 */
const int* asr_class(int* args);
const int* core_class(int* handler);
const int* dev_class(int* args);
const int* direct_class(int* args);
const int* fault_class(int* args);
const int* hex_class(int* args);
const int* oem_class(int* args);
const int* token_class(int* args);
const int* usbpd_class(int* args);

/* ---- 家族层 API (2026-09-28 全跨式整合) ---- */
uint8_t    hw_main_mode(void);                /* 六模式探测 (缓存一次) */
const char* hw_main_mode_str(uint8_t mode);    /* 模式名 */
uint8_t     hw_main_class_count(void);         /* = 9 */
const char* hw_main_class_name(uint8_t idx);   /* "ASR".."TOKEN"/"USBPD" 或 "?" */
int         hw_main_class_id(uint8_t idx);     /* 类 ID 或 -1 */
int         hw_main_find(int cls);             /* 类 ID → 表序, 未挂 -1 */
uint32_t    hw_main_checksum(void);            /* 类表 FNV-1a-32 黄金校验和 */
int         hw_main_probe_one(uint8_t idx);    /* 单类健康探针 (无副作用/弱链接) */
int         hw_main_probe_all(int (*putf)(const char*)); /* 全家族探针 → 失败类数 */
int         hw_main_cmd(const char* cmd, void* arg);     /* 字符串命令分发 (VM/CLI 同路) */
int         hw_main_selftest(int (*putf)(const char*));  /* 自检 → 失败数 (0=全过) */
void        hw_main_init(void* arg);           /* 上电: 黄金自证 + 探针缓存复位 */
void        hw_main_hook(void* arg);           /* 无操作 (对齐家族 hook 名) */

/* ---- CLI: ./xiaomo main [card|selftest|count|idx N|find ID|mode|sum|ok|probe ID|probeall] ---- */
int         hw_main_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* HW_MAIN_H */
