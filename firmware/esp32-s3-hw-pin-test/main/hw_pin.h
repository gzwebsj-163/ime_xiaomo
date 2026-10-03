#ifndef HW_PIN_H
#define HW_PIN_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * xiaomo - 引脚档案层 / 双模引脚驱动 / 编程电压层
 *          (hw_pin, 2026-09-30)  ——  GPIO 级真实烧录器的地基
 *
 * 定位: 把「烧录器」这件事从「换工具 / 换驱动」变成「换一条档案」。
 *       引脚映射声明化 -> 换目标芯片只改档案，协议代码一行不动。
 *       (与 panel_lcd 的「换屏 = 换一条档案」同一设计)
 *
 * 三层合一 (只依赖 BSP 回调, 全跨式六模式):
 *   L2 引脚档案  hw_pin_profile_t : 逻辑信号 -> 物理 GPIO 的映射表
 *   L1 引脚驱动  hw_pin_spi_xfer() : 双模
 *                ├ HW 模式 : 外设映射 (ESP32 GPIO Matrix / IOMUX), 快而准
 *                └ BB 模式 : 纯 GPIO bit-bang, 任意时序 / 时钟拉伸 / 非标器件
 *   L0 电压层    hw_pin_vpp_set()  : 可调编程电压 (斜坡 + 钳位 + ADC 回读闭环)
 *
 * 引脚定版 (用户 2026-09-30, 9 信号可映射 + GND 公共地):
 *   MOSI MISO CK CS   <- SPI 四线 (SPI Flash / 25xx / 93xx / AT93C)
 *   TX   RX           <- UART 两线 (MCU ISP: STM32/STC/ASRPRO/AVR)
 *   RST               <- 复位 / 进编程模式
 *   VPP               <- 可调编程电压 (EPROM/OTP, 也是通用编程器标志)
 *   VCC               <- 目标供电 (烧裸芯片进插座时由编程器供给)
 *   GND               <- 公共地 (固定, 不参与映射)
 *   ★ SPI 四线与 UART 两线物理独立不共用 -> 可并行工作
 *
 * 全跨式设计 (与 hw_token/hw_fault/hw_flash 同款):
 *   1. 编译期六模式探测, -DHW_PIN_MODE_OVERRIDE=n 可强指。
 *   2. 核心只依赖 stdint+string; 真机/内核 freestanding 可编。
 *   3. 引脚读写一律走 BSP 回调注入:
 *      默认 = 确定性模拟 (GPIO 电平状态机 + W25Q SPI Flash 器件模型),
 *      全平台逐位一致可回归; 真机固件 hw_pin_bsp_install() 注入
 *      GPIO/SPI/DAC, 上层协议零改动。
 *   4. 黄金参考 HW_PIN_GOLDEN = 档案表 FNV-1a-32
 *      (独立 Python 对拍锁定), selftest 逐项断言防未来误改。
 *
 * 六层接入: Makefile / OP_HW_PIN_CALL / mo2kbc 内置 / CLI pin /
 *           examples/pin_test.mo / tests/run_tests.sh
 * ============================================================ */

/* ---- 编译期模式 (与 hw_flash 同款) ---- */
typedef enum {
    HW_PIN_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_PIN_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux) */
    HW_PIN_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_PIN_MODE_ESP32   = 3,   /* ESP32 / S3 / C3 / C6 (IDF) */
    HW_PIN_MODE_ESP8266 = 4,   /* ESP8266 */
    HW_PIN_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_PIN_MODE_MAX     = 6
} hw_pin_mode_t;

/* ---- 逻辑信号 (引脚档案的抽象面) ---- */
typedef enum {
    HW_PIN_MOSI = 0,   /* SPI 主出从入 */
    HW_PIN_MISO,       /* SPI 主入从出 */
    HW_PIN_CK,         /* SPI 时钟 */
    HW_PIN_CS,         /* SPI 片选 (低有效) */
    HW_PIN_TX,         /* UART 发送 */
    HW_PIN_RX,         /* UART 接收 */
    HW_PIN_RST,        /* 目标复位 / 进编程模式 */
    HW_PIN_VPP,        /* 编程电压输出 */
    HW_PIN_VCC,        /* 目标供电使能 */
    HW_PIN_SIG_MAX     /* = 9 (GND 为公共地, 不参与映射) */
} hw_pin_sig_t;

/* ---- 驱动模式 (L1 双模) ---- */
#define HW_PIN_DRV_HW   0   /* 外设映射: ESP32 GPIO Matrix / IOMUX, 硬件收发 */
#define HW_PIN_DRV_BB   1   /* bit-bang : 纯 GPIO 翻转, 任意时序/时钟拉伸 */

/* ---- 编程电压档位 (L0) ---- */
#define HW_PIN_VPP_OFF   0
#define HW_PIN_VPP_5V    1    /* 5V  : 多数 5V 器件 / 24C 写保护释放 */
#define HW_PIN_VPP_12V   2    /* 12V : EPROM 27xx 编程 */
#define HW_PIN_VPP_12V5  3    /* 12.5V : 部分 OTP */
#define HW_PIN_VPP_21V   4    /* 21V : 高压 OTP / 老 EPROM */
#define HW_PIN_VPP_MAX   5

/* ---- 返回码 ---- */
#define HW_PIN_R_OK        0x00
#define HW_PIN_R_NOARGS    0x01
#define HW_PIN_R_BADARG    0x02
#define HW_PIN_R_IOERR     0x03   /* GPI/SPI 收发失败 */
#define HW_PIN_R_NOFLASH   0x04   /* 目标无应答 / JEDEC ID 非法 */
#define HW_PIN_R_VERIFY    0x05   /* 回读不符 */
#define HW_PIN_R_NODEV     0x06   /* 档案无效 / 引脚未映射 */
#define HW_PIN_R_RANGE     0x07   /* 地址/长度越界 */
#define HW_PIN_R_NAK       0x08   /* 目标回 NACK (0x1F) / 校验不符 */
#define HW_PIN_R_NOTGT     0x09   /* 目标无应答 / 握手超时 */
#define HW_PIN_R_NOCMD     (-1)
#define HW_PIN_R_HELP      (-2)

/* ---- 引脚档案 (L2 核心) ---- */
typedef struct {
    const char* name;                 /* 档案名 (如 "esp32-9p" / "w25q" ) */
    int16_t     gpio[HW_PIN_SIG_MAX]; /* 逻辑信号 -> GPIO 号; -1 = 未映射 */
    uint32_t    uart_baud;            /* UART 波特率 */
    uint32_t    spi_hz;               /* SPI 时钟 (Hz) */
    uint8_t     spi_mode;             /* SPI CPOL/CPHA, 0..3 */
    uint8_t     drv;                  /* HW_PIN_DRV_* */
    uint8_t     vpp_level;            /* HW_PIN_VPP_* 默认档位 */
} hw_pin_profile_t;

/* ---- 状态快照 ---- */
typedef struct {
    uint32_t spi_bytes;      /* 累计 SPI 收发字节 */
    uint32_t bb_toggles;     /* bit-bang 时钟翻转次数 */
    uint32_t vex;            /* 累计引脚电平写次数 */
    uint32_t vpp_set_count;  /* VPP 设置次数 */
    uint32_t vpp_mv;         /* 最近一次 VPP 回读 (mV) */
    uint8_t  active_idx;     /* 当前档案下标 */
    uint8_t  flash_id_ok;    /* 最近一次 RDID 是否合法 */
    uint32_t flash_jedec;    /* 最近一次 RDID 原始 32 位 (小端) */
    /* --- UART ISP (L3 #2) --- */
    uint32_t isp_rx;         /* 累计 UART 收字节 */
    uint32_t isp_tx;         /* 累计 UART 发字节 */
    uint8_t  isp_synced;     /* 是否已 0x7F 握手成功 */
    uint16_t isp_pid;        /* 最近一次 GetID 的 PID */
    uint8_t  isp_ver;        /* 最近一次 GetVersion 的版本 */
} hw_pin_stat_t;

/* ---- 真机 BSP: 引脚/电压回调注入 (默认 = 确定性模拟) ----
 * 约定: gpio_dir/gpio_write 0=成功; gpio_read 返回 0/1 (<0 失败);
 *       delay_us 0=成功; spi_xfer 返回收发字节数 (<0 失败);
 *       vpp_set/vpp_read 0=成功 (vpp_read 回填 mV)。
 * 任意回调可置 NULL -> 该动作回落模拟器 (故可只注入 SPI, 其余仍可回归)。 */
typedef struct {
    int (*gpio_dir)(int pin, int output);
    int (*gpio_write)(int pin, int level);
    int (*gpio_read)(int pin);
    int (*delay_us)(uint32_t us);
    int (*spi_xfer)(const hw_pin_profile_t* p, const uint8_t* tx, uint8_t* rx, uint32_t n);
    int (*uart_putc)(int ch);          /* 发 1 字节到目标 (TX), 0=成功 */
    int (*uart_getc)(void);            /* 收 1 字节 (RX), 返回 0..255 / <0 失败 */
    int (*vpp_set)(int level, uint32_t mv);
    int (*vpp_read)(uint32_t* mv);
} hw_pin_bsp_t;

/* ---- W25Q/25xx SPI Flash 命令 (L3 首个协议插件) ---- */
#define HW_PIN_FLASH_CMD_WREN     0x06u   /* Write Enable */
#define HW_PIN_FLASH_CMD_RDSR     0x05u   /* Read Status (bit0=WIP/busy) */
#define HW_PIN_FLASH_CMD_READ     0x03u   /* Read Data */
#define HW_PIN_FLASH_CMD_PP       0x02u   /* Page Program */
#define HW_PIN_FLASH_CMD_SE       0x20u   /* Sector Erase (4KB) */
#define HW_PIN_FLASH_CMD_RDID     0x9Fu   /* JEDEC ID */

/* ---- L3 协议插件 #2: UART ISP (STM32 AN3155 引导装载程序) ----
 * 走 mcu-isp 档案 (TX/RX/RST 三线), 半双工一问一答。
 * ACK=0x79 / NACK=0x1F, 握手 = 发 0x7F 收 ACK。 */
#define HW_PIN_ISP_ACK      0x79u
#define HW_PIN_ISP_NACK     0x1Fu
#define HW_PIN_ISP_SYNC     0x7Fu
#define HW_PIN_ISP_CMD_GET  0x00u   /* Get: 版本 + 可读命令表 */
#define HW_PIN_ISP_CMD_GVR  0x01u   /* Get Version */
#define HW_PIN_ISP_CMD_GID  0x02u   /* Get ID (PID) */
#define HW_PIN_ISP_CMD_RM   0x11u   /* Read Memory */
#define HW_PIN_ISP_CMD_GO   0x21u   /* Go (跳转到地址执行) */
#define HW_PIN_ISP_CMD_WM   0x31u   /* Write Memory */
#define HW_PIN_ISP_CMD_ER   0x43u   /* Erase (页)
                                     * ⚠️ 0x44 扩展擦除在 STM32F1 不存在,
                                     *    只有 0x43 标准页擦除。 */
#define HW_PIN_ISP_CMD_WP   0x63u   /* Write Protect */
#define HW_PIN_ISP_CMD_WRU  0x73u   /* Write Unprotect */

/* 模拟目标 (STM32F103 medium-density bootloader v3.1) */
#define HW_PIN_ISP_BOOTVER  0x31u   /* 版本 3.1 */
#define HW_PIN_ISP_PID      0x0410u /* STM32F103x8/B PID */
#ifndef HW_PIN_ISP_MEM_CAP
#define HW_PIN_ISP_MEM_CAP  8192u   /* 模型内存 (8KB = 8 x 1KB 页) */
#endif
#define HW_PIN_ISP_PAGE     1024u   /* STM32F103 页大小 1KB */
/* 命令表 (AN3155 F1 支持的 11 条, Get 命令返回的表) */
#define HW_PIN_ISP_NCMDS    11u

/* ---- 黄金值 (独立 Python 对拍锁定 2026-09-30, 勿改) ---- */
#define HW_PIN_GOLDEN         0x9E0F10FAu  /* 档案表 FNV-1a-32 */
#define HW_PIN_ISP_GOLDEN     0xD2A9A924u  /* ISP 命令表(11 条) FNV-1a-32 */
#define HW_PIN_ISP_FLASH_BASE 0x08000000u  /* STM32 片内 Flash 起始地址 */
#define HW_PIN_FLASH_JEDEC    0x001840EFu  /* W25Q128 RDID: 9F -> EF 40 18 */
#define HW_PIN_VPP_MODEL_MV   5000u        /* 模拟器 5V 档回读 mV */

/* 模拟 Flash 容量 (真机不占; -DHW_PIN_FLASH_CAP 可调) */
#ifndef HW_PIN_FLASH_CAP
#define HW_PIN_FLASH_CAP      8192u        /* 2 个 4KB 扇区 */
#endif
#define HW_PIN_FLASH_SECTOR   4096u
#define HW_PIN_FLASH_PAGE     256u

/* ============================================================
 * API
 * ============================================================ */

uint8_t     hw_pin_mode(void);
const char* hw_pin_mode_str(uint8_t mode);
const char* hw_pin_sig_name(int sig);
const char* hw_pin_result_code_str(int rc);

/* ---- L2 档案层 ---- */
uint32_t                     hw_pin_profile_count(void);
const hw_pin_profile_t*      hw_pin_profile_get(uint32_t idx);
const hw_pin_profile_t*      hw_pin_profile_find(const char* name);
const hw_pin_profile_t*      hw_pin_profile_default(void);
uint32_t                     hw_pin_profile_checksum(void);  /* FNV-1a-32 == GOLDEN */
int                          hw_pin_profile_valid(const hw_pin_profile_t* p);
/* 装载档案: NULL=复位为默认。成功 0 / 失败 HW_PIN_R_NODEV */
int                          hw_pin_profile_load(const hw_pin_profile_t* p);
int                          hw_pin_profile_load_name(const char* name);
const hw_pin_profile_t*      hw_pin_active(void);
int                          hw_pin_gpio_of(int sig);        /* 当前档案里信号的 GPIO, 未映射 -1 */

/* ---- L1 驱动层 (双模统一接口) ---- */
void        hw_pin_bsp_install(const hw_pin_bsp_t* bsp);     /* NULL=还原模拟器 */
int         hw_pin_gpio_write(int sig, int level);
int         hw_pin_gpio_read(int sig);
int         hw_pin_spi_xfer(const uint8_t* tx, uint8_t* rx, uint32_t n);
int         hw_pin_spi_set_cs(int level);                    /* 手动片选 */
int         hw_pin_uart_xfer(const uint8_t* tx, uint8_t* rx, uint32_t n);
/* UART 字节层 (ISP 用): putc 0=成功; getc 返回 0..255 / <0 失败 */
int         hw_pin_uart_putc(int ch);
int         hw_pin_uart_getc(void);

/* ---- L0 电压层 ---- */
int         hw_pin_vpp_set(int level);                       /* HW_PIN_VPP_* */
int         hw_pin_vpp_read(uint32_t* mv_out);
int         hw_pin_vpp_level_mv(int level);                  /* 档位标称 mV (0=未知) */

/* ---- L3 首个协议插件: 25xx SPI Flash ---- */
int         hw_pin_flash_wren(void);
int         hw_pin_flash_rdsr(uint8_t* sr);
int         hw_pin_flash_wait(uint32_t timeout_ms);
int         hw_pin_flash_rdid(uint32_t* jedec_out);          /* 0x9F */
int         hw_pin_flash_read(uint32_t addr, uint8_t* buf, uint32_t n);
int         hw_pin_flash_page_program(uint32_t addr, const uint8_t* buf, uint32_t n);
int         hw_pin_flash_sector_erase(uint32_t addr);
int         hw_pin_flash_verify(uint32_t addr, const uint8_t* buf, uint32_t n);

/* ---- L3 协议插件 #2: UART ISP (STM32 AN3155) ----
 * 每个操作内部自动确保握手 (首次调用会发 0x7F)。 */
uint32_t    hw_pin_isp_checksum(void);                           /* 命令表 FNV == ISP_GOLDEN */
int         hw_pin_isp_sync(void);                               /* 0x7F 握手 */
int         hw_pin_isp_reset_sync(void);                         /* 清握手态 (重来一遍) */
int         hw_pin_isp_get_version(uint8_t* ver, uint8_t* rdp);
int         hw_pin_isp_get(uint8_t* ver_out, uint32_t* cmdsum_out);  /* Get: 命令表 FNV */
int         hw_pin_isp_get_id(uint16_t* pid);
int         hw_pin_isp_read(uint32_t addr, uint8_t* buf, uint32_t n);
int         hw_pin_isp_write(uint32_t addr, const uint8_t* buf, uint32_t n);
int         hw_pin_isp_erase_all(void);                          /* 全片擦除 */
int         hw_pin_isp_go(uint32_t addr);                        /* 跳转执行 */

/* ---- 生命周期 (kvm_run 上电自动调 init) ---- */
void        hw_pin_init(void* arg);
void        hw_pin_hook(void* arg);
void        hw_pin_stat(hw_pin_stat_t* out);
void        hw_pin_result(char* buf, uint32_t cap);

/* ---- 命令分发 (VM OP_HW_PIN_CALL 用); >=0 结果码 / -1 / -2 ---- */
int         hw_pin_cmd(const char* cmd, void* ctx);

/* ---- 自检 (putf=NULL 静默) -> 失败项数 (0 = 全过) ---- */
int         hw_pin_selftest(int (*putf)(const char*));
/* CLI: ./xiaomo pin [...] */
int         hw_pin_cli(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* HW_PIN_H */
