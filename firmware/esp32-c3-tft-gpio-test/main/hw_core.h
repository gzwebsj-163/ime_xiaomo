/*
 * hw_core.h - xiaomo 内核 DNA 编码层 (hw_core, 全跨式六模式)
 *
 * DNA 编码体系 (用户设计, 值锁定勿改):
 *   基类: SIGNAL=0x0000 / DATA=0x0D00 / FLOW=0x0E00 / TERM=0x0F00
 *   10 项内核 DNA = 基类 | 子码, 表序: DEF DFF SIN SNG SID DFT DDT ODT FFQ TTU
 *   黄金校验和 = 该表 FNV-1a-32 (独立 Python 对拍锁定 2026-09-28):
 *     HW_CORE_GOLDEN = 0xA5618C4A  (selftest 逐项断言防未来误改)
 *
 * 全跨式六模式 (同 hw_fault/hw_token 家族):
 *   HOST / LINUX / KELL(TinyEMU 真内核) / ESP32 / ESP8266 / TEST
 *   0=HOST 1=LINUX 2=KELL 3=ESP32 4=ESP8266 5=TEST
 *   构建系统可 -DHW_CORE_MODE_OVERRIDE=n 强指; KELL 须 -DHW_CORE_KELL=1 抢先
 *   (KELL 内核内 __linux__ 亦被定义, 宏优先级必须 KELL 先于 linux)。
 *
 * 依赖纪律: 仅 stdint + string 标准函数, 无 malloc/无 pthread/无文件 IO,
 *   真机 (ESP32/ESP8266/内核嵌入) 可 -ffreestanding 编译。
 *
 * ⚠️ 2026-09-28 整合说明: 原草案的 AUTO/AUTO_PTR 宏 (__auto_type GNU 私有
 *   扩展) 与 C++ auto 返回函数已移除 — 家族纪律要求 gcc C11 与 g++ C++17
 *   双编译输出逐位一致, GNU 私有扩展会破坏交叉编译器兼容。CORE_DNA_FOREACH
 *   保留并改为标准 C99 for 声明写法, 原 core_init/core_deinit/core_ject/
 *   core_deject 四个槽位 API 保留 (语义修复: core_init 返回码)。
 */
#ifndef HW_CORE_H
#define HW_CORE_H
#include <stdint.h>

/* ---- DNA 基类编码 ---- */
#define HW_CORE_BASE_SIGNAL 0x0000U
#define HW_CORE_BASE_DATA   0x0D00U
#define HW_CORE_BASE_FLOW   0x0E00U
#define HW_CORE_BASE_TERM   0x0F00U

/* ---- DNA 子码 ---- */
#define HW_CORE_SUB_EF    0x00efU
#define HW_CORE_SUB_DD    0x00ddU
#define HW_CORE_SUB_EE    0x00eeU
#define HW_CORE_SUB_59    0x0059U
#define HW_CORE_SUB_80    0x0080U
#define HW_CORE_SUB_100   0x0100U
#define HW_CORE_SUB_FF    0x00ffU
#define HW_CORE_SUB_FD    0x00fdU

/* ---- 10 项内核 DNA (值锁定, 勿改) ---- */
#define HW_CORE_DEF    (HW_CORE_BASE_DATA | HW_CORE_SUB_EF)
#define HW_CORE_DFF    (HW_CORE_BASE_DATA | HW_CORE_SUB_FF)
#define HW_CORE_SIN    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_EF)
#define HW_CORE_SNG    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_DD)
#define HW_CORE_SID    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_EE)
#define HW_CORE_DFT    (HW_CORE_BASE_SIGNAL | 0x0059U)
#define HW_CORE_DDT    (HW_CORE_BASE_SIGNAL | 0x0080U)
#define HW_CORE_ODT    (HW_CORE_BASE_SIGNAL | 0x0100U)
#define HW_CORE_FFQ    (HW_CORE_BASE_FLOW | HW_CORE_SUB_FF)
#define HW_CORE_TTU    (HW_CORE_BASE_TERM | HW_CORE_SUB_FD)

#ifndef HW_CORE_MAX
#define HW_CORE_MAX 10U
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CORE_DNA_DEF = HW_CORE_DEF,
    CORE_DNA_DFF = HW_CORE_DFF,
    CORE_DNA_SIN = HW_CORE_SIN,
    CORE_DNA_SNG = HW_CORE_SNG,
    CORE_DNA_SID = HW_CORE_SID,
    CORE_DNA_DFT = HW_CORE_DFT,
    CORE_DNA_DDT = HW_CORE_DDT,
    CORE_DNA_ODT = HW_CORE_ODT,
    CORE_DNA_FFQ = HW_CORE_FFQ,
    CORE_DNA_TTU = HW_CORE_TTU,
} core_dna;

/* ---- 六模式编号 (全跨式家族统一) ---- */
#define HW_CORE_MODE_HOST    0
#define HW_CORE_MODE_LINUX   1
#define HW_CORE_MODE_KELL    2
#define HW_CORE_MODE_ESP32   3
#define HW_CORE_MODE_ESP8266 4
#define HW_CORE_MODE_TEST    5
#define HW_CORE_MODE_MAX     6

/* ---- 模式探测 (编译期宏, 运行期缓存一次) ---- */
uint8_t hw_core_mode(void);
const char* hw_core_mode_str(uint8_t mode);

/* ---- DNA 表 (只读访问) ---- */
uint8_t hw_core_dna_count(void);            /* = HW_CORE_MAX */
const core_dna* hw_core_dna_table(void);    /* 10 项表基址 */
const char* hw_core_dna_name(uint8_t idx);  /* DEF/DFF/.../TTU 或 "?" */
const char* hw_core_base_name(uint16_t code);/* 编码 → 基类名 SIGNAL/DATA/FLOW/TERM */
uint32_t hw_core_checksum(void);            /* FNV-1a-32 黄金 (0xA5618C4A) */

/* ---- 家族 hook 对 (VM 上电/复位约定, 本模块无动态资源) ---- */
void hw_core_init(void* arg);               /* 上电: 黄金自证 + 槽位复位 */
void hw_core_hook(void* arg);               /* 无操作 (对齐家族 hook 名) */

/* ---- 槽位 API (用户原设计, 语义修复版) ---- */
/* 装载 10 项 DNA 编码到 handler[0..9]; 返回 0 成功 / -1 handler 为 NULL */
int core_init(int handler[]);
/* 清零 uint16 槽位数组 (内部槽与外部 handler 均可清) */
void core_deinit(uint16_t handler[]);
/* 内部槽位: 装载 (0=成功/-1 槽号非法) / 卸载 / 已用计数 */
int core_slot_load(uint8_t slot, core_dna code);
int core_slot_free(uint8_t slot);
uint32_t core_slots_used(void);
/* 弹出槽 0 的 DNA 编码 (空槽/NULL → 0) */
core_dna core_ject(const int point[]);
core_dna core_deject(const uint16_t point[]);

/* ---- 命令分发 (VM OP_HW_CORE_CALL / CLI 共用) ----
 * 返回: >=0 结果 / -1 未识别或参数非法 / -2 help
 *   count        → 10 (DNA 项数)
 *   ok           → 1 (黄金校验和匹配) / 0 (不匹配!)
 *   idx N        → 第 N 项 DNA 编码 (0x0DEF=3567 等)
 *   slot N CODE  → 槽位装载 → 0
 *   free N       → 槽位卸载 → 0
 *   slots        → 已用槽位数
 *   mode         → 模式编号
 *   help         → -2
 */
int hw_core_cmd(const char* cmd, void* ctx);

/* ---- 自检 (putf=NULL 静默只返回失败数, 真机固件用) ---- */
int hw_core_selftest(int (*putf)(const char*));

/* ---- CLI: ./xiaomo core [card|dna|idx N|slot N CODE|free N|slots|mode|selftest] ---- */
int hw_core_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif

/* 标准 C99 for 声明写法 (原 __auto_type GNU 私有扩展已移除, 全编译器兼容) */
#define CORE_DNA_FOREACH(it) \
    for (uint32_t (it) = 0U; (it) < HW_CORE_MAX; ++(it))

#endif /* HW_CORE_H */
