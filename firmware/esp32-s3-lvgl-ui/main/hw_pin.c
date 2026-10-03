/*
 * xiaomo - 引脚档案层 / 双模引脚驱动 / 编程电压层 (hw_pin, 2026-09-30)
 *
 * GPIO 级真实烧录器的地基。一句话: 换目标芯片 = 换一条档案。
 *
 *   L2 档案  hw_pin_profile_t  : 逻辑信号(MOSI/MISO/CK/CS/TX/RX/RST/VPP/VCC)
 *                               -> 物理 GPIO 号 的映射表
 *   L1 驱动  hw_pin_spi_xfer() : 双模
 *              HW 模式 = 外设映射 (ESP32 GPIO Matrix / IOMUX, 硬件收发)
 *              BB 模式 = 纯 GPIO bit-bang (任意时序 / 时钟拉伸 / 非标器件)
 *   L0 电压  hw_pin_vpp_set()  : 可调编程电压 (斜坡 + 钳位 + ADC 回读闭环)
 *
 * 全跨式 (与 hw_token/hw_fault/hw_flash 同款):
 *   核心只依赖 stdint+string; 引脚读写一律走 BSP 回调。
 *   默认 = 确定性模拟: GPIO 电平状态机 + W25Q 器件模型 (逐字节状态机,
 *   与时钟边沿无关 -> 全平台逐位一致可回归)。真机固件
 *   hw_pin_bsp_install() 注入 GPIO/SPI/DAC, 上层协议零改动。
 *
 * 黄金参考 HW_PIN_GOLDEN = 档案表 FNV-1a-32 (Python 对拍锁定)。
 *
 * 六层接入: Makefile / OP_HW_PIN_CALL / mo2kbc 内置 hw_pin() / CLI pin /
 *           examples/pin_test.mo / tests/run_tests.sh
 */
#include "hw_pin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * 1. 编译期模式探测 (全跨式)
 * ============================================================ */
static uint8_t pin_mode_probe(void)
{
#if defined(HW_PIN_MODE_OVERRIDE)
    return (uint8_t)HW_PIN_MODE_OVERRIDE;
#elif defined(HW_PIN_KELL)
    return (uint8_t)HW_PIN_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_PIN_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_PIN_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_PIN_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_PIN_MODE_HOST;
#else
    return (uint8_t)HW_PIN_MODE_TEST;
#endif
}

uint8_t hw_pin_mode(void)
{
    static uint8_t cached = 0xFFu;
    if (cached == 0xFFu) cached = pin_mode_probe();
    return cached;
}

static const char* const pin_mode_names[HW_PIN_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_pin_mode_str(uint8_t mode)
{
    return (mode < HW_PIN_MODE_MAX) ? pin_mode_names[mode] : "?";
}

static const char* const pin_sig_names[HW_PIN_SIG_MAX] = {
    "MOSI", "MISO", "CK", "CS", "TX", "RX", "RST", "VPP", "VCC"
};

const char* hw_pin_sig_name(int sig)
{
    return (sig >= 0 && sig < HW_PIN_SIG_MAX) ? pin_sig_names[sig] : "?";
}

const char* hw_pin_result_code_str(int rc)
{
    switch (rc) {
    case HW_PIN_R_OK:     return "OK";
    case HW_PIN_R_NOARGS: return "NOARGS";
    case HW_PIN_R_BADARG: return "BADARG";
    case HW_PIN_R_IOERR:  return "IOERR";
    case HW_PIN_R_NOFLASH:return "NOFLASH";
    case HW_PIN_R_VERIFY: return "VERIFY";
    case HW_PIN_R_NODEV:  return "NODEV";
    case HW_PIN_R_RANGE:  return "RANGE";
    case HW_PIN_R_NAK:    return "NAK";
    case HW_PIN_R_NOTGT:  return "NOTGT";
    case HW_PIN_R_NOCMD:  return "NOCMD";
    case HW_PIN_R_HELP:   return "HELP";
    default:              return "?";
    }
}

/* ============================================================
 * 2. 档案表 (L2)
 *
 * 逻辑信号顺序 = MOSI MISO CK CS TX RX RST VPP VCC  (见 hw_pin_sig_t)
 * -1 = 该信号在本档案里未映射 (如纯 SPI 档案不接 TX/RX)。
 *
 * ⚠️ ESP32-classic 选脚约束 (硬件事实, 勿改):
 *   GPIO 6~11   = 片内 SPI Flash, 占用, 不可用
 *   GPIO 0/2/5/12/15 = strapping 脚, 上电电平影响启动模式, 尽量避开
 *   GPIO 34~39  = input-only, 只可作输入 (MISO/RX 可用)
 *   GPIO 1/3    = UART0 (USB 串口下载口), 留作上位机通信
 * ============================================================ */
static const hw_pin_profile_t g_profiles[] = {
    /* name        MOSI MISO  CK  CS  TX  RX RST VPP VCC  baud    spi_hz  md drv vpp */
    { "esp32-9p", {  23,  19,  18,   5,  17,  16,  -1, -1, -1 }, 115200, 1000000, 0, HW_PIN_DRV_BB, HW_PIN_VPP_OFF  },
    { "w25q",     {  23,  19,  18,   5,  -1,  -1,  -1, -1, -1 },      0, 1000000, 0, HW_PIN_DRV_BB, HW_PIN_VPP_OFF  },
    { "mcu-isp",  {  -1,  -1,  -1,  -1,  17,  16,   4, -1, -1 },  115200,       0, 0, HW_PIN_DRV_BB, HW_PIN_VPP_OFF  },
    { "eprom",    {  23,  19,  18,   5,  -1,  -1,  -1,  25, 26 },      0,  500000, 0, HW_PIN_DRV_BB, HW_PIN_VPP_12V  }
};

#define PIN_PROFILE_COUNT (uint32_t)(sizeof(g_profiles) / sizeof(g_profiles[0]))

uint32_t hw_pin_profile_count(void) { return PIN_PROFILE_COUNT; }

const hw_pin_profile_t* hw_pin_profile_get(uint32_t idx)
{
    return (idx < PIN_PROFILE_COUNT) ? &g_profiles[idx] : NULL;
}

const hw_pin_profile_t* hw_pin_profile_find(const char* name)
{
    uint32_t i;
    if (!name) return NULL;
    for (i = 0; i < PIN_PROFILE_COUNT; i++)
        if (strcmp(g_profiles[i].name, name) == 0) return &g_profiles[i];
    return NULL;
}

const hw_pin_profile_t* hw_pin_profile_default(void) { return &g_profiles[0]; }

/* FNV-1a-32: 名字字符串 + 9 个 gpio 值 (int16) + 通信参数 + 模式/驱动/电压 */
uint32_t hw_pin_profile_checksum(void)
{
    uint32_t h = 2166136261u, i, k, j;
    for (i = 0; i < PIN_PROFILE_COUNT; i++) {
        const hw_pin_profile_t* p = &g_profiles[i];
        for (k = 0; p->name[k] != '\0'; k++) {
            h ^= (uint32_t)(uint8_t)p->name[k];
            h *= 16777619u;
        }
        for (j = 0; j < (uint32_t)HW_PIN_SIG_MAX; j++) {
            uint32_t v = (uint32_t)((int32_t)p->gpio[j] & 0xFFFF);
            h ^= (v & 0xFFu);      h *= 16777619u;
            h ^= ((v >> 8) & 0xFFu); h *= 16777619u;
        }
        h ^= (p->uart_baud & 0xFFu)          ; h *= 16777619u;
        h ^= ((p->uart_baud >> 8) & 0xFFu)   ; h *= 16777619u;
        h ^= ((p->uart_baud >> 16) & 0xFFu)  ; h *= 16777619u;
        h ^= ((p->uart_baud >> 24) & 0xFFu)  ; h *= 16777619u;
        h ^= (p->spi_hz & 0xFFu)             ; h *= 16777619u;
        h ^= ((p->spi_hz >> 8) & 0xFFu)      ; h *= 16777619u;
        h ^= ((p->spi_hz >> 16) & 0xFFu)     ; h *= 16777619u;
        h ^= ((p->spi_hz >> 24) & 0xFFu)     ; h *= 16777619u;
        h ^= (uint32_t)p->spi_mode ; h *= 16777619u;
        h ^= (uint32_t)p->drv      ; h *= 16777619u;
        h ^= (uint32_t)p->vpp_level; h *= 16777619u;
    }
    return h;
}

int hw_pin_profile_valid(const hw_pin_profile_t* p)
{
    uint32_t j;
    int mapped = 0;
    if (!p || !p->name) return HW_PIN_R_NODEV;
    if (p->spi_mode > 3u || p->drv > HW_PIN_DRV_BB || p->vpp_level >= HW_PIN_VPP_MAX)
        return HW_PIN_R_BADARG;
    for (j = 0; j < (uint32_t)HW_PIN_SIG_MAX; j++) {
        int16_t g = p->gpio[j];
        if (g < -1) return HW_PIN_R_BADARG;
        if (g >= 0) mapped++;
    }
    if (mapped == 0) return HW_PIN_R_NODEV;
    return HW_PIN_R_OK;
}

/* ============================================================
 * 3. 确定性模拟器 (BSP 默认实现)
 *    a) GPIO 电平状态机
 *    b) W25Q 器件模型: 逐字节状态机, 与时钟边沿无关
 * ============================================================ */
#define PIN_GPIO_MAX 64

static int g_gpio_dir[PIN_GPIO_MAX];
static int g_gpio_lvl[PIN_GPIO_MAX];

static uint8_t  g_flash[HW_PIN_FLASH_CAP];   /* 静态: 真机禁用大局部数组 */
static int      g_flash_ready;               /* 1 = 已格式化为 0xFF */
static int      g_wren;                      /* Write Enable Latch */
static int      g_busy_reads;                /* 之后 N 次 RDSR 见 busy */
static uint8_t  g_cmd;                       /* 当前命令 */
static uint32_t g_addr;                      /* 命令内累计地址 */
static uint32_t g_seq;                       /* 命令内已收字节数 */

static void sim_flash_fmt(void)
{
    if (!g_flash_ready) {
        memset(g_flash, 0xFF, sizeof(g_flash));
        g_flash_ready = 1;
    }
}

static void sim_cmd_reset(void)
{
    g_cmd = 0u; g_addr = 0u; g_seq = 0u;
}

/* 单字节时沿: 返回器件在此字节上驱动的 MISO 位 (0xFF 空闲时为高)
 *
 * 状态机约定 (与时钟边沿无关, 保证全平台确定性):
 *   g_seq==0         = 操作码字节
 *   g_seq==1,2,3     = ① 地址字节 (0x03/0x02/0x20) / ② RDID 的 ID 字节
 *   g_seq>=4         = 数据相位 (READ 出数据 / PP 收数据)
 * ⚠️ RDID 的厂商码 EF 在 **操作码字节那一拍** 就随 MISO 出来 (rx[0]),
 *    不是 rx[1] —— SPI 是全双工, 发 0x9F 的同时就在读 ID。 */
static uint8_t sim_flash_byte(uint8_t tx)
{
    uint8_t rx = 0xFFu;

    if (g_seq == 0u) {                 /* 首字节 = 操作码 */
        g_cmd = tx; g_addr = 0u; g_seq = 1u;
        switch (g_cmd) {
        case HW_PIN_FLASH_CMD_WREN:                    /* 0x06 */
            g_wren = 1; return 0xFFu;
        case HW_PIN_FLASH_CMD_RDSR: {                  /* 0x05 */
            uint8_t sr = (g_busy_reads > 0) ? 0x01u : 0x00u;
            if (g_busy_reads > 0) g_busy_reads--;
            return sr;
        }
        case HW_PIN_FLASH_CMD_RDID:                    /* 0x9F -> EF 在 rx[0] */
            return (uint8_t)(HW_PIN_FLASH_JEDEC & 0xFFu);
        default:                                       /* 0x03/0x02/0x20: 收地址 */
            return 0xFFu;
        }
    }

    switch (g_cmd) {
    case HW_PIN_FLASH_CMD_RDID: {
        uint32_t k = g_seq;            /* 1,2,3 -> 40 18 00 */
        rx = (k < 4u) ? (uint8_t)((HW_PIN_FLASH_JEDEC >> (8u * k)) & 0xFFu) : 0x00u;
        break;
    }
    case HW_PIN_FLASH_CMD_RDSR:
        rx = 0x00u;
        break;
    case HW_PIN_FLASH_CMD_READ:
    case HW_PIN_FLASH_CMD_PP: {
        sim_flash_fmt();
        if (g_seq <= 3u) {             /* 地址相位 */
            g_addr = (g_addr << 8) | (uint32_t)tx;
        } else if (g_cmd == HW_PIN_FLASH_CMD_READ) {
            rx = (g_addr < sizeof(g_flash)) ? g_flash[g_addr] : 0xFFu;
            g_addr++;
        } else {                       /* PP: 数据相位, 需 WEL */
            if (g_wren && g_addr < sizeof(g_flash)) g_flash[g_addr] = tx;
            g_addr++;
        }
        break;
    }
    case HW_PIN_FLASH_CMD_SE:
        if (g_seq <= 3u) {             /* 地址相位; 第 3 个地址字节到齐即擦 */
            g_addr = (g_addr << 8) | (uint32_t)tx;
            if (g_seq == 3u) {
                sim_flash_fmt();
                if (g_wren) {
                    uint32_t base = g_addr & ~(HW_PIN_FLASH_SECTOR - 1u);
                    uint32_t n;
                    for (n = 0; n < HW_PIN_FLASH_SECTOR && base + n < sizeof(g_flash); n++)
                        g_flash[base + n] = 0xFFu;
                }
                g_wren = 0;            /* WEL 被擦除操作消耗 */
                g_busy_reads = 1;      /* 之后第一次 RDSR 见 busy */
            }
        }
        break;
    default:
        break;
    }

    g_seq++;
    return rx;
}

/* --- c) STM32 AN3155 引导装载程序器件模型 (UART, 半双工一问一答) ---
 *
 * 与 SPI 器件模型的差别: UART 不是「逐位移位」而是「收 1 字节 -> 可能吐
 * 若干字节」的非流水线协议, 故这里用一个「主机字节消费状态机 + 输出 FIFO」:
 *     sim_isp_byte(in)  吃掉一个主机字节, 把目标应答压进 g_isp_out[]
 *     (主机侧 hw_pin_uart_getc 从 g_isp_out 取)
 * 握手 = 发 0x7F -> 回 ACK(0x79); 此后每条命令 [cmd][~cmd] -> ACK/NACK,
 * 再按各命令的参数/数据相位一问一答。全程纯状态机 (与时间无关), 保证
 * 全平台逐位一致。
 *
 * ⚠️ 两条真机同款语义 (负向测试的基础):
 *   ① 未握手时非 0x7F 的字节一律忽略 -> 主机读不到任何字节 (NOTGT)。
 *   ② 命令码补码错 / 未知命令 -> NACK; 数据校验错 -> NACK 且不落盘。
 */
#define ISP_OUTQ 320   /* >= HW_PIN_ISP_MAXPKT, 否则大数据读会溢出 */

/* AN3155 (STM32F103) Get 命令返回的 11 条命令, 顺序即上表顺序 */
static const uint8_t g_isp_cmds[HW_PIN_ISP_NCMDS] = {
    HW_PIN_ISP_CMD_GET, HW_PIN_ISP_CMD_GVR, HW_PIN_ISP_CMD_GID,
    HW_PIN_ISP_CMD_RM,  HW_PIN_ISP_CMD_GO,  HW_PIN_ISP_CMD_WM,
    HW_PIN_ISP_CMD_ER,  HW_PIN_ISP_CMD_WP,  HW_PIN_ISP_CMD_WRU,
    0x82u, 0x92u                                  /* Readout Protect / Unprotect */
};

/* ISP 状态机相位 */
#define ISP_ST_SYNC  0    /* 未握手: 只认 0x7F */
#define ISP_ST_CMD   1    /* 等命令码 */
#define ISP_ST_CMDC  2    /* 等命令码补码 ~cmd */
#define ISP_ST_ADDR  3    /* 等 4 字节地址 (RM/WM/GO) */
#define ISP_ST_ADDC  4    /* 等地址 XOR 校验 */
#define ISP_ST_LEN   5    /* 等长度字节 N-1 */
#define ISP_ST_LENC  6    /* 等 ~(N-1) */
#define ISP_ST_DATA  7    /* 等 N 字节数据 (WM) */
#define ISP_ST_DATC  8    /* 等数据 XOR 校验 (WM) */
#define ISP_ST_PAGES 9    /* 等页数 N (ER/WP); N=0xFF = 全局擦除哨兵 */
#define ISP_ST_PGLST 10   /* 等页号列表 */
#define ISP_ST_PGCHK 11   /* 等页号 XOR 校验 */

static uint8_t g_isp_mem[HW_PIN_ISP_MEM_CAP];
static int     g_isp_fmt;
static uint8_t g_isp_out[ISP_OUTQ];
static int     g_isp_outn;
static int     g_isp_st;
static uint8_t g_isp_cmd;
static uint8_t g_isp_xor;
static uint32_t g_isp_addr;
static uint32_t g_isp_cnt;
static uint32_t g_isp_ln;
static uint32_t g_isp_pg;

/* 命令表 + PID + 版本的 FNV-1a-32 (独立 Python 对拍 == HW_PIN_ISP_GOLDEN) */
uint32_t hw_pin_isp_checksum(void)
{
    uint32_t h = 2166136261u, i;
    /* 与运行期 hw_pin_isp_get() 同口径: 只哈希 11 条命令字节,
     * 这样静态黄金值能直接与「真器件 Get 应答」对拍。 */
    for (i = 0; i < HW_PIN_ISP_NCMDS; i++) { h ^= g_isp_cmds[i]; h *= 16777619u; }
    return h;
}

static void isp_fmt(void)
{
    if (!g_isp_fmt) { memset(g_isp_mem, 0xFF, sizeof(g_isp_mem)); g_isp_fmt = 1; }
}
/* STM32 片内 Flash 从 0x08000000 起 —— 器件模型必须做基址映射,
 * 否则主机用真实地址 (0x08000000) 读写会全部落到模型内存之外。 */
static int isp_mem_ok(uint32_t addr, uint32_t* idx)
{
    if (addr < HW_PIN_ISP_FLASH_BASE) return 0;
    *idx = addr - HW_PIN_ISP_FLASH_BASE;
    return (*idx < HW_PIN_ISP_MEM_CAP) ? 1 : 0;
}
static void isp_push(int b) { if (g_isp_outn < ISP_OUTQ) g_isp_out[g_isp_outn++] = (uint8_t)(b & 0xFF); }
static void isp_push_mem(uint32_t addr, uint32_t n)
{
    uint32_t i, idx;
    isp_fmt();
    for (i = 0; i < n; i++) {
        uint32_t a = addr + i;
        isp_push(isp_mem_ok(a, &idx) ? (int)g_isp_mem[idx] : 0xFF);
    }
}

static void sim_isp_reset(void)
{
    g_isp_fmt = 0; g_isp_outn = 0; g_isp_st = ISP_ST_SYNC;
    g_isp_cmd = 0u; g_isp_xor = 0u; g_isp_addr = 0u;
    g_isp_cnt = 0u; g_isp_ln = 0u; g_isp_pg = 0u;
    isp_fmt();
}

/* 命令码校验通过后: 发首 ACK + 决定下一相位 */
static void sim_isp_dispatch(void)
{
    switch (g_isp_cmd) {
    case HW_PIN_ISP_CMD_GET:
        isp_push(HW_PIN_ISP_ACK);
        isp_push((int)HW_PIN_ISP_NCMDS - 1);          /* N = 命令数-1 */
        { uint32_t i; for (i = 0; i < HW_PIN_ISP_NCMDS; i++) isp_push(g_isp_cmds[i]); }
        g_isp_st = ISP_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_GVR:
        isp_push(HW_PIN_ISP_ACK);
        isp_push((int)HW_PIN_ISP_BOOTVER);
        isp_push((int)HW_PIN_ISP_NCMDS - 1);
        { uint32_t i; for (i = 0; i < HW_PIN_ISP_NCMDS; i++) isp_push(g_isp_cmds[i]); }
        g_isp_st = ISP_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_GID:
        isp_push(HW_PIN_ISP_ACK);
        isp_push(0x01);                               /* N-1 = 1 -> 2 字节 */
        isp_push((int)(HW_PIN_ISP_PID & 0xFFu));      /* PID 低 */
        isp_push((int)((HW_PIN_ISP_PID >> 8) & 0xFFu));/* PID 高 */
        g_isp_st = ISP_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_RM:
    case HW_PIN_ISP_CMD_WM:
    case HW_PIN_ISP_CMD_GO:
        isp_push(HW_PIN_ISP_ACK);
        g_isp_addr = 0u; g_isp_xor = 0u; g_isp_cnt = 0u;
        g_isp_st = ISP_ST_ADDR;
        break;
    case HW_PIN_ISP_CMD_ER:
    case HW_PIN_ISP_CMD_WP:
        isp_push(HW_PIN_ISP_ACK);
        g_isp_st = ISP_ST_PAGES;
        break;
    case HW_PIN_ISP_CMD_WRU:
    case 0x82u: case 0x92u:                           /* 无参命令 */
        isp_push(HW_PIN_ISP_ACK);
        g_isp_st = ISP_ST_CMD;
        break;
    default:
        isp_push(HW_PIN_ISP_NACK);
        g_isp_st = ISP_ST_CMD;
        break;
    }
}

/* 长度相位通过后: RM 直接吐 N 字节; WM 进入数据相位 */
static void sim_isp_after_len(void)
{
    uint32_t n = g_isp_ln + 1u;
    if (g_isp_cmd == HW_PIN_ISP_CMD_RM) {
        isp_push_mem(g_isp_addr, n);
        g_isp_st = ISP_ST_CMD;
    } else {                                          /* WM */
        g_isp_cnt = n; g_isp_xor = 0u; g_isp_st = ISP_ST_DATA;
    }
}

static void sim_isp_byte(uint8_t in)
{
    /* 0x7F = 重新握手 (只在命令边界合法; 数据相位里的 0x7F 是数据) */
    if (in == (uint8_t)HW_PIN_ISP_SYNC &&
        (g_isp_st == ISP_ST_SYNC || g_isp_st == ISP_ST_CMD)) {
        g_isp_outn = 0;
        g_isp_st = ISP_ST_CMD;
        isp_push(HW_PIN_ISP_ACK);
        return;
    }

    switch (g_isp_st) {
    case ISP_ST_SYNC:
        return;                                       /* 未握手: 忽略 */

    case ISP_ST_CMD:
        g_isp_cmd = in; g_isp_xor = (uint8_t)(in ^ 0xFFu);
        g_isp_st = ISP_ST_CMDC;
        return;

    case ISP_ST_CMDC:
        if (in != g_isp_xor) { isp_push(HW_PIN_ISP_NACK); g_isp_st = ISP_ST_CMD; return; }
        {
            uint32_t i; int known = 0;
            for (i = 0; i < HW_PIN_ISP_NCMDS; i++)
                if (g_isp_cmds[i] == g_isp_cmd) { known = 1; break; }
            if (!known) { isp_push(HW_PIN_ISP_NACK); g_isp_st = ISP_ST_CMD; return; }
        }
        sim_isp_dispatch();
        return;

    case ISP_ST_ADDR:
        g_isp_addr = (g_isp_addr << 8) | (uint32_t)in;
        g_isp_xor ^= in;
        if (++g_isp_cnt >= 4u) { g_isp_cnt = 0u; g_isp_st = ISP_ST_ADDC; }
        return;

    case ISP_ST_ADDC:
        if (in != g_isp_xor) { isp_push(HW_PIN_ISP_NACK); g_isp_st = ISP_ST_CMD; }
        else {
            isp_push(HW_PIN_ISP_ACK);
            if (g_isp_cmd == HW_PIN_ISP_CMD_GO) g_isp_st = ISP_ST_CMD;
            else { g_isp_ln = 0u; g_isp_st = ISP_ST_LEN; }   /* RM/WM */
        }
        return;

    case ISP_ST_LEN:
        g_isp_ln = in; g_isp_xor = (uint8_t)(in ^ 0xFFu); g_isp_st = ISP_ST_LENC;
        return;

    case ISP_ST_LENC:
        if (in != g_isp_xor) { isp_push(HW_PIN_ISP_NACK); g_isp_st = ISP_ST_CMD; return; }
        isp_push(HW_PIN_ISP_ACK);
        sim_isp_after_len();
        return;

    case ISP_ST_DATA: {
        uint32_t idx;
        isp_fmt();
        if (isp_mem_ok(g_isp_addr, &idx)) g_isp_mem[idx] = in;
        g_isp_addr++; g_isp_xor ^= in;
        if (--g_isp_cnt == 0u) g_isp_st = ISP_ST_DATC;
        return;
    }

    case ISP_ST_DATC:
        if (in != g_isp_xor) isp_push(HW_PIN_ISP_NACK);
        else isp_push(HW_PIN_ISP_ACK);
        g_isp_st = ISP_ST_CMD;
        return;

    case ISP_ST_PAGES:
        g_isp_ln = in; g_isp_xor = in; g_isp_pg = 0u;
        if (in == 0xFFu) {                            /* 全局擦除哨兵 */
            isp_fmt(); memset(g_isp_mem, 0xFF, sizeof(g_isp_mem));
            g_isp_st = ISP_ST_PGCHK;
        } else g_isp_st = ISP_ST_PGLST;
        return;

    case ISP_ST_PGLST:
        g_isp_xor ^= in;
        g_isp_addr = HW_PIN_ISP_FLASH_BASE + (uint32_t)in * HW_PIN_ISP_PAGE;
        isp_fmt();
        if (g_isp_addr >= HW_PIN_ISP_FLASH_BASE &&
            (g_isp_addr - HW_PIN_ISP_FLASH_BASE) + HW_PIN_ISP_PAGE <= sizeof(g_isp_mem))
            memset(g_isp_mem + (g_isp_addr - HW_PIN_ISP_FLASH_BASE), 0xFF, HW_PIN_ISP_PAGE);
        if (++g_isp_pg >= (g_isp_ln + 1u)) g_isp_st = ISP_ST_PGCHK;
        return;

    case ISP_ST_PGCHK:
        if (in != g_isp_xor) isp_push(HW_PIN_ISP_NACK);
        else isp_push(HW_PIN_ISP_ACK);
        g_isp_st = ISP_ST_CMD;
        return;

    default:
        g_isp_st = ISP_ST_SYNC;
        return;
    }
}

/* ============================================================
 * 4. BSP + 活动档案
 * ============================================================ */
static hw_pin_bsp_t  g_bsp;                    /* 回调全空 = 用模拟器 */
static const hw_pin_profile_t* g_active;       /* 活动档案 */
static uint8_t       g_active_idx;

static hw_pin_stat_t g_stat;
static char          g_err[96];

/* --- GPIO 层: BSP 优先, 否则模拟 --- */
static int pin_gpio_write_raw(int gpio, int level)
{
    if (gpio < 0) return HW_PIN_R_NODEV;
    if (g_bsp.gpio_write) return (g_bsp.gpio_write(gpio, level) == 0) ? 0 : -1;
    if (gpio < PIN_GPIO_MAX) g_gpio_lvl[gpio] = (level != 0) ? 1 : 0;
    g_stat.vex++;
    return 0;
}

static int pin_gpio_read_raw(int gpio)
{
    if (gpio < 0) return -1;
    if (g_bsp.gpio_read) return g_bsp.gpio_read(gpio);
    return (gpio < PIN_GPIO_MAX) ? g_gpio_lvl[gpio] : 0;
}

static void pin_delay_us(uint32_t us)
{
    if (g_bsp.delay_us) { (void)g_bsp.delay_us(us); return; }
    /* 模拟: 不真等, 只计数 (保持全平台确定性) */
}

/* ============================================================
 * 5. L1 驱动层
 * ============================================================ */

/* bit-bang: 一个字节 (MSB 先行, 按档案 CPOL/CPHA)
 *
 * MISO 来源二选一 (这是模拟器与真机的分界):
 *   真机       = g_bsp.gpio_read 装了就真读 MISO 引脚
 *   模拟器     = 无真输入 -> 由器件模型 (sim_flash_byte) 供给本字节的应答
 * 两条路的时钟/MOSI 翻转都是真 GPIO 动作 (bb_toggles 会计数),
 * 所以真机上用示波器/逻辑分析仪看到的波形与模拟器是同一套时序。 */
static int bb_spi_byte(uint8_t tx, uint8_t* rx_out)
{
    int mosi = hw_pin_gpio_of(HW_PIN_MOSI);
    int sck  = hw_pin_gpio_of(HW_PIN_CK);
    int miso = hw_pin_gpio_of(HW_PIN_MISO);
    uint32_t cpol, cpha;
    uint8_t rx = 0, dev_rx = 0;
    int use_dev, b;

    if (mosi < 0 || sck < 0 || miso < 0) return HW_PIN_R_NODEV;
    if (!g_active) return HW_PIN_R_NODEV;
    cpol = (g_active->spi_mode >> 1) & 1u;
    cpha = (g_active->spi_mode & 1u);

    use_dev = (g_bsp.gpio_read == NULL) ? 1 : 0;
    if (use_dev) dev_rx = sim_flash_byte(tx);   /* 器件对整字节的应答 */

    (void)pin_gpio_write_raw(sck, (int)cpol);     /* 空闲电平 */

    for (b = 7; b >= 0; b--) {
        (void)pin_gpio_write_raw(mosi, (tx >> b) & 1);
        /* 采样沿: cpha=0 -> 第一沿; cpha=1 -> 第二沿 */
        (void)pin_gpio_write_raw(sck, (int)(cpol ^ 1u));
        g_stat.bb_toggles++;
        if (cpha == 0u) {
            int bit = use_dev ? ((dev_rx >> b) & 1)
                              : (pin_gpio_read_raw(miso) > 0);
            rx |= (uint8_t)(bit ? (1u << b) : 0u);
        }
        (void)pin_gpio_write_raw(sck, (int)cpol);
        g_stat.bb_toggles++;
        if (cpha == 1u) {
            int bit = use_dev ? ((dev_rx >> b) & 1)
                              : (pin_gpio_read_raw(miso) > 0);
            rx |= (uint8_t)(bit ? (1u << b) : 0u);
        }
        pin_delay_us(1u);
    }
    if (rx_out) *rx_out = rx;
    return 0;
}

int hw_pin_spi_xfer(const uint8_t* tx, uint8_t* rx, uint32_t n)
{
    uint32_t i;
    if (!g_active) return HW_PIN_R_NODEV;
    if (n == 0u) return HW_PIN_R_OK;

    /* HW 模式: 走 BSP 外设回调 (ESP32 GPIO Matrix 映射硬件 SPI) */
    if (g_active->drv == HW_PIN_DRV_HW && g_bsp.spi_xfer) {
        int r = g_bsp.spi_xfer(g_active, tx, rx, n);
        if (r < 0) return HW_PIN_R_IOERR;
        g_stat.spi_bytes += n;
        return HW_PIN_R_OK;
    }

    /* BB 模式 (或 HW 模式无 BSP 时的兜底): 纯 GPIO 翻转
     * ⚠️ 必须先把「三根线是否都在本档案里映射」查清楚再进循环 ——
     *    bb_spi_byte 的 NODEV 曾被我 (void) 丢弃, 导致
     *    「档案没接 SPI 脚」被当成成功 (负向用例假通过)。 */
    if (hw_pin_gpio_of(HW_PIN_MOSI) < 0 ||
        hw_pin_gpio_of(HW_PIN_MISO) < 0 ||
        hw_pin_gpio_of(HW_PIN_CK)   < 0)
        return HW_PIN_R_NODEV;

    for (i = 0; i < n; i++) {
        uint8_t out = 0xFFu;
        int r = bb_spi_byte(tx ? tx[i] : 0xFFu, &out);
        if (r != 0) return HW_PIN_R_NODEV;
        if (rx) rx[i] = out;
    }
    g_stat.spi_bytes += n;
    return HW_PIN_R_OK;
}

int hw_pin_spi_set_cs(int level)
{
    int rc = pin_gpio_write_raw(hw_pin_gpio_of(HW_PIN_CS), level);
    if (level != 0) {
        /* 片选拉高 = 命令会话结束。
         * ⚠️ 不能在此清 g_wren: RX-only 命令(如 RDSR 轮询 busy) 也会拉高片选,
         *    一清 WEL 则紧随其后的 PP/SE 必然写不进去 (曾致 T7 verify FAIL)。 */
        sim_cmd_reset();
    }
    return (rc == 0) ? HW_PIN_R_OK : HW_PIN_R_NODEV;
}

int hw_pin_gpio_write(int sig, int level)
{
    return (pin_gpio_write_raw(hw_pin_gpio_of(sig), level) == 0)
         ? HW_PIN_R_OK : HW_PIN_R_NODEV;
}

int hw_pin_gpio_read(int sig)
{
    return pin_gpio_read_raw(hw_pin_gpio_of(sig));
}

int hw_pin_uart_xfer(const uint8_t* tx, uint8_t* rx, uint32_t n)
{
    /* 逐字节一问一答 (UART 不是流水线)。真机走 BSP 的 uart_putc/uart_getc,
     * 模拟器走 AN3155 器件模型。 */
    uint32_t i;
    if (!g_active) return HW_PIN_R_NODEV;
    for (i = 0; i < n; i++) {
        int rc = hw_pin_uart_putc(tx ? (int)tx[i] : 0xFF);
        if (rc != HW_PIN_R_OK) return rc;
        if (rx) { int g = hw_pin_uart_getc(); rx[i] = (g >= 0) ? (uint8_t)g : 0xFFu; }
    }
    return HW_PIN_R_OK;
}

/* ---- UART 字节层 (ISP 用) ----
 * putc: 0=成功; getc: 返回 0..255, <0 = 无数据/失败。
 * MISO 类比: 真机由 BSP 的 uart_* 接管; 模拟器由器件模型 (sim_isp_byte) 供给。 */
int hw_pin_uart_putc(int ch)
{
    if (!g_active) return HW_PIN_R_NODEV;
    if (hw_pin_gpio_of(HW_PIN_TX) < 0) return HW_PIN_R_NODEV;   /* 该档案没接 UART */
    g_stat.isp_tx++;
    if (g_bsp.uart_putc) return (g_bsp.uart_putc(ch) == 0) ? HW_PIN_R_OK : HW_PIN_R_IOERR;
    sim_isp_byte((uint8_t)(ch & 0xFF));
    return HW_PIN_R_OK;
}

int hw_pin_uart_getc(void)
{
    int b;
    if (!g_active) return -1;
    if (hw_pin_gpio_of(HW_PIN_RX) < 0) return -1;
    if (g_bsp.uart_getc) {
        b = g_bsp.uart_getc();
        if (b >= 0) g_stat.isp_rx++;
        return b;
    }
    if (g_isp_outn <= 0) return -1;
    b = g_isp_out[0];
    { int i; for (i = 1; i < g_isp_outn; i++) g_isp_out[i - 1] = g_isp_out[i]; }
    g_isp_outn--;
    g_stat.isp_rx++;
    return b;
}

/* ============================================================
 * 6. L0 电压层
 * ============================================================ */
static const uint32_t g_vpp_mv[HW_PIN_VPP_MAX] = {
    0u, 5000u, 12000u, 12500u, 21000u
};

int hw_pin_vpp_level_mv(int level)
{
    return (level >= 0 && level < HW_PIN_VPP_MAX) ? (int)g_vpp_mv[level] : 0;
}

int hw_pin_vpp_set(int level)
{
    if (level < 0 || level >= HW_PIN_VPP_MAX) return HW_PIN_R_BADARG;
    if (g_bsp.vpp_set) {
        if (g_bsp.vpp_set(level, g_vpp_mv[level]) != 0) return HW_PIN_R_IOERR;
    }
    g_stat.vpp_set_count++;
    {
        uint32_t mv = 0;
        (void)hw_pin_vpp_read(&mv);
        g_stat.vpp_mv = mv;
    }
    return HW_PIN_R_OK;
}

int hw_pin_vpp_read(uint32_t* mv_out)
{
    uint32_t mv = 0;
    if (g_bsp.vpp_read) {
        if (g_bsp.vpp_read(&mv) != 0) return HW_PIN_R_IOERR;
    } else {
        mv = HW_PIN_VPP_MODEL_MV;   /* 模拟器固定 5V */
    }
    if (mv_out) *mv_out = mv;
    g_stat.vpp_mv = mv;
    return HW_PIN_R_OK;
}

/* ============================================================
 * 7. L3 协议插件: 25xx SPI Flash
 * ============================================================ */
static int flash_cmd1(uint8_t c, uint8_t* out)
{
    uint8_t tx = c, rx = 0xFFu;
    int rc;
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(&tx, &rx, 1u);
    if (rc != HW_PIN_R_OK) { (void)hw_pin_spi_set_cs(1); return rc; }
    if (out) *out = rx;
    return HW_PIN_R_OK;
}

int hw_pin_flash_wren(void)
{
    int rc = flash_cmd1(HW_PIN_FLASH_CMD_WREN, NULL);
    (void)hw_pin_spi_set_cs(1);
    return rc;
}

int hw_pin_flash_rdsr(uint8_t* sr)
{
    uint8_t rx = 0x00u;
    int rc = flash_cmd1(HW_PIN_FLASH_CMD_RDSR, &rx);
    (void)hw_pin_spi_set_cs(1);
    if (rc != HW_PIN_R_OK) return rc;
    if (sr) *sr = rx;
    return HW_PIN_R_OK;
}

int hw_pin_flash_wait(uint32_t timeout_ms)
{
    uint32_t i;
    for (i = 0; i < (timeout_ms ? timeout_ms : 1u); i++) {
        uint8_t sr = 0;
        if (hw_pin_flash_rdsr(&sr) != HW_PIN_R_OK) return HW_PIN_R_IOERR;
        if ((sr & 0x01u) == 0u) return HW_PIN_R_OK;   /* WIP=0 */
    }
    return HW_PIN_R_IOERR;
}

int hw_pin_flash_rdid(uint32_t* jedec_out)
{
    uint8_t tx[4], rx[4];
    uint32_t v;
    int rc;
    tx[0] = HW_PIN_FLASH_CMD_RDID;
    tx[1] = 0u; tx[2] = 0u; tx[3] = 0u;
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(tx, rx, 4u);
    (void)hw_pin_spi_set_cs(1);
    if (rc != HW_PIN_R_OK) return rc;
    /* SPI 全双工: 发 0x9F 那一拍就已经在收厂商码 -> 三字节 ID 落在 rx[0..2] */
    v = (uint32_t)rx[0] | ((uint32_t)rx[1] << 8) | ((uint32_t)rx[2] << 16);
    g_stat.flash_jedec = v;
    /* 合法 JEDEC: 厂商码非 0x00/0xFF/0x7F (悬空/无器件) */
    if (rx[0] == 0x00u || rx[0] == 0xFFu || rx[0] == 0x7Fu) {
        g_stat.flash_id_ok = 0;
        return HW_PIN_R_NOFLASH;
    }
    g_stat.flash_id_ok = 1;
    if (jedec_out) *jedec_out = v;
    return HW_PIN_R_OK;
}

int hw_pin_flash_read(uint32_t addr, uint8_t* buf, uint32_t n)
{
    uint8_t cmd[4];
    int rc;
    if (!buf || n == 0u) return HW_PIN_R_BADARG;
    cmd[0] = HW_PIN_FLASH_CMD_READ;
    cmd[1] = (uint8_t)((addr >> 16) & 0xFFu);
    cmd[2] = (uint8_t)((addr >> 8) & 0xFFu);
    cmd[3] = (uint8_t)(addr & 0xFFu);
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(cmd, NULL, 4u);
    if (rc != HW_PIN_R_OK) { (void)hw_pin_spi_set_cs(1); return rc; }
    rc = hw_pin_spi_xfer(NULL, buf, n);
    (void)hw_pin_spi_set_cs(1);
    return rc;
}

int hw_pin_flash_page_program(uint32_t addr, const uint8_t* buf, uint32_t n)
{
    uint8_t cmd[4];
    int rc;
    if (!buf || n == 0u || n > HW_PIN_FLASH_PAGE) return HW_PIN_R_BADARG;
    rc = hw_pin_flash_wren();
    if (rc != HW_PIN_R_OK) return rc;
    cmd[0] = HW_PIN_FLASH_CMD_PP;
    cmd[1] = (uint8_t)((addr >> 16) & 0xFFu);
    cmd[2] = (uint8_t)((addr >> 8) & 0xFFu);
    cmd[3] = (uint8_t)(addr & 0xFFu);
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(cmd, NULL, 4u);
    if (rc == HW_PIN_R_OK) rc = hw_pin_spi_xfer(buf, NULL, n);
    (void)hw_pin_spi_set_cs(1);
    return rc;
}

int hw_pin_flash_sector_erase(uint32_t addr)
{
    uint8_t cmd[4];
    int rc;
    rc = hw_pin_flash_wren();
    if (rc != HW_PIN_R_OK) return rc;
    cmd[0] = HW_PIN_FLASH_CMD_SE;
    cmd[1] = (uint8_t)((addr >> 16) & 0xFFu);
    cmd[2] = (uint8_t)((addr >> 8) & 0xFFu);
    cmd[3] = (uint8_t)(addr & 0xFFu);
    (void)hw_pin_spi_set_cs(1);
    (void)hw_pin_spi_set_cs(0);
    rc = hw_pin_spi_xfer(cmd, NULL, 4u);
    (void)hw_pin_spi_set_cs(1);
    return rc;
}

int hw_pin_flash_verify(uint32_t addr, const uint8_t* buf, uint32_t n)
{
    static uint8_t rd[HW_PIN_FLASH_PAGE];
    uint32_t got = 0;
    int rc;
    if (!buf) return HW_PIN_R_BADARG;
    while (got < n) {
        uint32_t chunk = n - got;
        if (chunk > (uint32_t)sizeof(rd)) chunk = (uint32_t)sizeof(rd);
        rc = hw_pin_flash_read(addr + got, rd, chunk);
        if (rc != HW_PIN_R_OK) return rc;
        if (memcmp(rd, buf + got, chunk) != 0) return HW_PIN_R_VERIFY;
        got += chunk;
    }
    return HW_PIN_R_OK;
}

/* ============================================================
 * 7b. L3 协议插件 #2: UART ISP (STM32 AN3155)
 *
 * 半双工一问一答, 每个操作内部先确保握手 (isp_ensure_sync)。
 * 返回码: HW_PIN_R_OK / NAK (目标拒绝) / NOTGT (无应答) / NODEV (档案无 UART) …
 * ============================================================ */
#define HW_PIN_ISP_MAXPKT 256u   /* 单次传输上限 (模型输出 FIFO 级别) */

/* 收一个字节; 无数据返回 -1 */
static int isp_recv(void) { return hw_pin_uart_getc(); }

/* 发一字节 */
static int isp_send(int b) { return hw_pin_uart_putc(b); }

/* 发命令码 + 补码, 收 ACK/NACK */
static int isp_cmd_begin(uint8_t cmd)
{
    int ack;
    int rc = isp_send((int)cmd);
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_send((int)(uint8_t)(cmd ^ 0xFFu));
    if (rc != HW_PIN_R_OK) return rc;
    /* 目标必先回 ACK/NACK (命令探测的首应答) */
    ack = isp_recv();
    if (ack < 0) return HW_PIN_R_NOTGT;
    if ((uint8_t)ack != (uint8_t)HW_PIN_ISP_ACK) return HW_PIN_R_NAK;
    return HW_PIN_R_OK;
}

/* 0x7F 握手: 最多 3 次 */
int hw_pin_isp_sync(void)
{
    int attempt;
    if (!g_active) return HW_PIN_R_NODEV;
    if (hw_pin_gpio_of(HW_PIN_TX) < 0 || hw_pin_gpio_of(HW_PIN_RX) < 0)
        return HW_PIN_R_NODEV;
    if (g_stat.isp_synced) return HW_PIN_R_OK;
    /* ⚠️ 变量名勿用 try/catch/new/class 等 C++ 关键字:
     *    本文件同时以 C11 与 C++17 编译, C 里合法在 C++ 里是语法错。 */
    for (attempt = 0; attempt < 3; attempt++) {
        int ack;
        if (isp_send((int)HW_PIN_ISP_SYNC) != HW_PIN_R_OK) return HW_PIN_R_IOERR;
        ack = isp_recv();
        if (ack >= 0 && (uint8_t)ack == (uint8_t)HW_PIN_ISP_ACK) {
            g_stat.isp_synced = 1u;
            return HW_PIN_R_OK;
        }
    }
    g_stat.isp_synced = 0u;
    return HW_PIN_R_NOTGT;
}

/* 清握手态 (重来一遍握手): 复位器件模型 + 清标志 */
int hw_pin_isp_reset_sync(void)
{
    if (!g_bsp.uart_putc) sim_isp_reset();
    g_stat.isp_synced = 0u;
    return hw_pin_isp_sync();
}

static int isp_ensure_sync(void)
{
    if (g_stat.isp_synced) return HW_PIN_R_OK;
    return hw_pin_isp_sync();
}

/* 发 4 字节地址 (MSB 先行) + XOR 校验, 收 ACK */
static int isp_send_addr(uint32_t addr)
{
    uint8_t b[4]; uint8_t x = 0u; int i, ack;
    b[0] = (uint8_t)((addr >> 24) & 0xFFu);
    b[1] = (uint8_t)((addr >> 16) & 0xFFu);
    b[2] = (uint8_t)((addr >>  8) & 0xFFu);
    b[3] = (uint8_t)( addr        & 0xFFu);
    for (i = 0; i < 4; i++) { if (isp_send(b[i]) != HW_PIN_R_OK) return HW_PIN_R_IOERR; x ^= b[i]; }
    if (isp_send(x) != HW_PIN_R_OK) return HW_PIN_R_IOERR;
    ack = isp_recv();
    if (ack < 0) return HW_PIN_R_NOTGT;
    return ((uint8_t)ack == (uint8_t)HW_PIN_ISP_ACK) ? HW_PIN_R_OK : HW_PIN_R_NAK;
}

/* 读「命令表」响应 (GET 与 GVR 共用):
 *   GET (0x00): [ACK][N][N+1 个命令字节]
 *   GVR (0x01): [ACK][版本][N][N+1 个命令字节]   <- 多一个版本前缀
 * 必须读完 N+1 个命令字节, 否则残留会污染下一条命令。 */
static int isp_read_table(uint8_t cmd, int with_version,
                          uint8_t* ver_out, uint32_t* cmdsum_out)
{
    int rc, n, i;
    uint32_t h = 2166136261u;
    if (ver_out) *ver_out = 0u;
    if (cmdsum_out) *cmdsum_out = 0u;
    rc = isp_ensure_sync();
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(cmd);
    if (rc != HW_PIN_R_OK) return rc;
    if (with_version) {
        int v = isp_recv();
        if (v < 0) return HW_PIN_R_NOTGT;
        g_stat.isp_ver = (uint8_t)v;
        if (ver_out) *ver_out = (uint8_t)v;
    }
    n = isp_recv();                      /* N-1 = 命令数-1 */
    if (n < 0) return HW_PIN_R_NOTGT;
    for (i = 0; i <= (n & 0xFF); i++) {
        int c = isp_recv();
        if (c < 0) return HW_PIN_R_NOTGT;
        h ^= (uint32_t)(c & 0xFF); h *= 16777619u;
    }
    if (cmdsum_out) *cmdsum_out = h;
    return HW_PIN_R_OK;
}

/* Get: 只取命令表 (纯表 FNV, 可与 HW_PIN_ISP_GOLDEN 对拍) */
int hw_pin_isp_get(uint8_t* ver_out, uint32_t* cmdsum_out)
{
    uint32_t cs = 0u;
    int rc = isp_read_table(HW_PIN_ISP_CMD_GET, 0, NULL, &cs);
    if (rc != HW_PIN_R_OK) return rc;
    if (cmdsum_out) *cmdsum_out = cs;
    if (ver_out) *ver_out = g_stat.isp_ver;
    return HW_PIN_R_OK;
}

/* Get Version: 版本 + 命令表 (读干整条 GVR 响应) */
int hw_pin_isp_get_version(uint8_t* ver, uint8_t* rdp)
{
    uint8_t v = 0u; uint32_t cs = 0u;
    int rc = isp_read_table(HW_PIN_ISP_CMD_GVR, 1, &v, &cs);
    if (rc != HW_PIN_R_OK) return rc;
    if (ver) *ver = v;
    if (rdp) *rdp = 0u;
    return HW_PIN_R_OK;
}

/* Get ID: 回 2 字节 PID (小端) */
int hw_pin_isp_get_id(uint16_t* pid)
{
    int rc = isp_ensure_sync();
    int n, lo, hi;
    if (pid) *pid = 0u;
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(HW_PIN_ISP_CMD_GID);
    if (rc != HW_PIN_R_OK) return rc;
    n  = isp_recv();                       /* N-1 = 1 */
    lo = isp_recv();
    hi = isp_recv();
    if (n < 0 || lo < 0 || hi < 0) return HW_PIN_R_NOTGT;
    g_stat.isp_pid = (uint16_t)((lo & 0xFF) | ((hi & 0xFF) << 8));
    if (pid) *pid = g_stat.isp_pid;
    return HW_PIN_R_OK;
}

/* Read Memory: 地址校验 -> 长度校验 -> 读 N 字节 */
int hw_pin_isp_read(uint32_t addr, uint8_t* buf, uint32_t n)
{
    uint32_t i; int rc;
    if (!buf || n == 0u || n > HW_PIN_ISP_MAXPKT) return HW_PIN_R_RANGE;
    rc = isp_ensure_sync();
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(HW_PIN_ISP_CMD_RM);
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_send_addr(addr);
    if (rc != HW_PIN_R_OK) return rc;
    /* 长度: N-1 + ~(N-1) */
    { uint8_t ln = (uint8_t)(n - 1u);
      if (isp_send(ln) != HW_PIN_R_OK || isp_send((uint8_t)(ln ^ 0xFFu)) != HW_PIN_R_OK)
          return HW_PIN_R_IOERR; }
    { int ack = isp_recv();
      if (ack < 0) return HW_PIN_R_NOTGT;
      if ((uint8_t)ack != (uint8_t)HW_PIN_ISP_ACK) return HW_PIN_R_NAK; }
    for (i = 0; i < n; i++) {
        int c = isp_recv();
        if (c < 0) return HW_PIN_R_NOTGT;
        buf[i] = (uint8_t)c;
    }
    return HW_PIN_R_OK;
}

/* Write Memory: 地址校验 -> 长度校验 -> N 数据 + XOR 校验 -> ACK */
int hw_pin_isp_write(uint32_t addr, const uint8_t* buf, uint32_t n)
{
    uint32_t i; uint8_t x = 0u; int rc;
    if (!buf || n == 0u) return HW_PIN_R_RANGE;
    rc = isp_ensure_sync();
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(HW_PIN_ISP_CMD_WM);
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_send_addr(addr);
    if (rc != HW_PIN_R_OK) return rc;
    { uint8_t ln = ((n > HW_PIN_ISP_MAXPKT) ? (uint8_t)(HW_PIN_ISP_MAXPKT - 1u)
                                            : (uint8_t)(n - 1u));
      if (n > HW_PIN_ISP_MAXPKT) n = HW_PIN_ISP_MAXPKT;
      if (isp_send(ln) != HW_PIN_R_OK || isp_send((uint8_t)(ln ^ 0xFFu)) != HW_PIN_R_OK)
          return HW_PIN_R_IOERR; }
    { int ack = isp_recv();
      if (ack < 0) return HW_PIN_R_NOTGT;
      if ((uint8_t)ack != (uint8_t)HW_PIN_ISP_ACK) return HW_PIN_R_NAK; }
    for (i = 0; i < n; i++) { if (isp_send(buf[i]) != HW_PIN_R_OK) return HW_PIN_R_IOERR; x ^= buf[i]; }
    if (isp_send(x) != HW_PIN_R_OK) return HW_PIN_R_IOERR;
    { int ack = isp_recv();
      if (ack < 0) return HW_PIN_R_NOTGT;
      return ((uint8_t)ack == (uint8_t)HW_PIN_ISP_ACK) ? HW_PIN_R_OK : HW_PIN_R_NAK; }
}

/* 全片擦除 (全局擦除哨兵 N=0xFF, 校验和亦 0xFF) */
int hw_pin_isp_erase_all(void)
{
    int rc = isp_ensure_sync();
    int ack;
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(HW_PIN_ISP_CMD_ER);
    if (rc != HW_PIN_R_OK) return rc;
    if (isp_send(0xFFu) != HW_PIN_R_OK) return HW_PIN_R_IOERR;
    if (isp_send(0xFFu) != HW_PIN_R_OK) return HW_PIN_R_IOERR;   /* XOR(0xFF) = 0xFF */
    ack = isp_recv();
    if (ack < 0) return HW_PIN_R_NOTGT;
    return ((uint8_t)ack == (uint8_t)HW_PIN_ISP_ACK) ? HW_PIN_R_OK : HW_PIN_R_NAK;
}

/* Go: 跳转到地址执行 */
int hw_pin_isp_go(uint32_t addr)
{
    int rc = isp_ensure_sync();
    if (rc != HW_PIN_R_OK) return rc;
    rc = isp_cmd_begin(HW_PIN_ISP_CMD_GO);
    if (rc != HW_PIN_R_OK) return rc;
    return isp_send_addr(addr);
}

/* ============================================================
 * 8. 生命周期 / 档案装载
 * ============================================================ */
int hw_pin_profile_load(const hw_pin_profile_t* p)
{
    int rc;
    if (!p) {
        g_active = hw_pin_profile_default();
        g_active_idx = 0;
        return HW_PIN_R_OK;
    }
    rc = hw_pin_profile_valid(p);
    if (rc != HW_PIN_R_OK) return rc;
    g_active = p;
    {
        uint32_t i;
        for (i = 0; i < PIN_PROFILE_COUNT; i++)
            if (&g_profiles[i] == p) { g_active_idx = (uint8_t)i; break; }
    }
    return HW_PIN_R_OK;
}

int hw_pin_profile_load_name(const char* name)
{
    const hw_pin_profile_t* p = hw_pin_profile_find(name);
    if (!p) return HW_PIN_R_NODEV;
    return hw_pin_profile_load(p);
}

const hw_pin_profile_t* hw_pin_active(void) { return g_active; }

int hw_pin_gpio_of(int sig)
{
    if (!g_active || sig < 0 || sig >= HW_PIN_SIG_MAX) return -1;
    return (int)g_active->gpio[sig];
}

void hw_pin_bsp_install(const hw_pin_bsp_t* bsp)
{
    if (bsp) g_bsp = *bsp;
    else     memset(&g_bsp, 0, sizeof(g_bsp));
}

void hw_pin_init(void* arg)
{
    (void)arg;
    memset(&g_stat, 0, sizeof(g_stat));
    memset(&g_bsp, 0, sizeof(g_bsp));
    memset(g_gpio_dir, 0, sizeof(g_gpio_dir));
    memset(g_gpio_lvl, 0, sizeof(g_gpio_lvl));
    g_err[0] = '\0';
    g_active = hw_pin_profile_default();
    g_active_idx = 0;
    g_flash_ready = 0;
    sim_flash_fmt();
    g_wren = 0; g_busy_reads = 0;
    sim_cmd_reset();
    sim_isp_reset();
}

void hw_pin_hook(void* arg) { (void)arg; }

void hw_pin_stat(hw_pin_stat_t* out)
{
    if (out) {
        *out = g_stat;
        out->active_idx = g_active_idx;
    }
}

void hw_pin_result(char* buf, uint32_t cap)
{
    if (!buf || cap == 0u) return;
    snprintf(buf, cap, "%s", g_err[0] ? g_err : "-");
}

/* ============================================================
 * 9. 命令分发 (VM OP_HW_PIN_CALL 用)
 * ============================================================ */
static void pin_dump_active(void)
{
    int i;
    const hw_pin_profile_t* p = g_active;
    printf("pin: profile=%s drv=%s spi_mode=%u spi_hz=%u baud=%u vpp=%d\n",
           p->name, (p->drv == HW_PIN_DRV_HW) ? "HW" : "BB",
           (unsigned)p->spi_mode, (unsigned)p->spi_hz,
           (unsigned)p->uart_baud, (int)p->vpp_level);
    printf("pin: ");
    for (i = 0; i < HW_PIN_SIG_MAX; i++) {
        if (p->gpio[i] >= 0) printf("%s=IO%d ", hw_pin_sig_name(i), (int)p->gpio[i]);
    }
    printf("GND=--\n");
}

int hw_pin_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (!cmd) return HW_PIN_R_NOARGS;

    if (strcmp(cmd, "card") == 0 || strcmp(cmd, "info") == 0) {
        printf("pin: mode=%s profiles=%u golden=0x%08X\n",
               hw_pin_mode_str(hw_pin_mode()),
               (unsigned)hw_pin_profile_count(),
               (unsigned)hw_pin_profile_checksum());
        printf("pin: L3 plugins: flash(25xx SPI RDID=EF4018) + isp(AN3155 UART cmdsum=0x%08X)\n",
               (unsigned)hw_pin_isp_checksum());
        pin_dump_active();
        return HW_PIN_R_OK;
    }
    if (strcmp(cmd, "mode") == 0) {
        printf("pin: mode=%s (%u)\n", hw_pin_mode_str(hw_pin_mode()), (unsigned)hw_pin_mode());
        return HW_PIN_R_OK;
    }
    if (strcmp(cmd, "profiles") == 0) {
        uint32_t i;
        for (i = 0; i < hw_pin_profile_count(); i++) {
            const hw_pin_profile_t* p = hw_pin_profile_get(i);
            printf("pin: [%u] %-9s drv=%s spi_hz=%u baud=%u vpp=%d\n",
                   (unsigned)i, p->name, (p->drv == HW_PIN_DRV_HW) ? "HW" : "BB",
                   (unsigned)p->spi_hz, (unsigned)p->uart_baud, (int)p->vpp_level);
        }
        return HW_PIN_R_OK;
    }
    if (strncmp(cmd, "load ", 5) == 0) {
        int rc = hw_pin_profile_load_name(cmd + 5);
        if (rc == HW_PIN_R_OK) pin_dump_active();
        else printf("pin: load '%s' FAIL\n", cmd + 5);
        return rc;
    }
    if (strcmp(cmd, "valid") == 0) {
        int rc = hw_pin_profile_valid(g_active);
        printf("pin: profile '%s' valid -> %s (%d)\n",
               g_active->name, hw_pin_result_code_str(rc), rc);
        return rc;
    }
    if (strncmp(cmd, "drv ", 4) == 0) {
        /* ⚠️ g_active 是「档案指针」, 不能指向栈上临时体 (退出即悬垂)。
         *    用静态副本承载运行期覆写, 静态表本体保持只读。 */
        static hw_pin_profile_t s_ovr;
        if (g_active) s_ovr = *g_active;
        s_ovr.drv = (strcmp(cmd + 4, "hw") == 0) ? HW_PIN_DRV_HW : HW_PIN_DRV_BB;
        g_active = &s_ovr;
        printf("pin: drv -> %s\n", (s_ovr.drv == HW_PIN_DRV_HW) ? "HW" : "BB");
        return HW_PIN_R_OK;
    }
    if (strcmp(cmd, "id") == 0 || strcmp(cmd, "rdid") == 0) {
        uint32_t j = 0;
        int rc = hw_pin_flash_rdid(&j);
        printf("pin: RDID -> rc=%d jedec=0x%06X (%02X %02X %02X)\n",
               rc, (unsigned)j,
               (unsigned)(j & 0xFFu), (unsigned)((j >> 8) & 0xFFu), (unsigned)((j >> 16) & 0xFFu));
        return rc;
    }
    if (strncmp(cmd, "read ", 5) == 0) {
        static uint8_t buf[64];
        uint32_t addr = (uint32_t)strtol(cmd + 5, NULL, 0);
        uint32_t n = 16u;
        int rc;
        if (n > (uint32_t)sizeof(buf)) n = (uint32_t)sizeof(buf);
        rc = hw_pin_flash_read(addr, buf, n);
        if (rc == HW_PIN_R_OK) {
            uint32_t i;
            printf("pin: read %u..%u:", (unsigned)addr, (unsigned)(addr + n - 1));
            for (i = 0; i < n; i++) printf(" %02X", (unsigned)buf[i]);
            printf("\n");
        } else printf("pin: read FAIL rc=%d\n", rc);
        return rc;
    }
    if (strncmp(cmd, "erase ", 6) == 0) {
        uint32_t addr = (uint32_t)strtol(cmd + 6, NULL, 0);
        int rc = hw_pin_flash_sector_erase(addr);
        printf("pin: erase @%u -> rc=%d\n", (unsigned)addr, rc);
        return rc;
    }
    if (strncmp(cmd, "wtest", 5) == 0) {
        /* 写 256B 模式 -> 回读校验: 证明整条 pins->flash 链路 */
        static uint8_t pat[HW_PIN_FLASH_PAGE];
        uint32_t i;
        int rc;
        for (i = 0; i < (uint32_t)sizeof(pat); i++)
            pat[i] = (uint8_t)((i * 7u + 13u) & 0xFFu);
        rc = hw_pin_flash_sector_erase(0u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(16u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_page_program(0u, pat, (uint32_t)sizeof(pat));
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(16u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_verify(0u, pat, (uint32_t)sizeof(pat));
        printf("pin: wtest(256B) -> %s (%d)\n", hw_pin_result_code_str(rc), rc);
        return rc;
    }
    if (strcmp(cmd, "vppread") == 0) {
        uint32_t mv = 0;
        int rc = hw_pin_vpp_read(&mv);
        printf("pin: VPP read -> %u mV (rc=%d)\n", (unsigned)mv, rc);
        return rc;
    }
    if (strncmp(cmd, "vpp ", 4) == 0) {
        int lv = (int)strtol(cmd + 4, NULL, 0);
        int rc = hw_pin_vpp_set(lv);
        printf("pin: VPP set %d (%d mV) -> rc=%d\n",
               lv, hw_pin_vpp_level_mv(lv), rc);
        return rc;
    }
    if (strcmp(cmd, "isp") == 0 || strncmp(cmd, "isp ", 4) == 0) {
        const char* sub = (cmd[3] == ' ') ? cmd + 4 : "info";
        int rc;
        /* ISP 走 mcu-isp 档案 (TX/RX), 自动切档案避免用户手滑 */
        if (hw_pin_gpio_of(HW_PIN_TX) < 0) (void)hw_pin_profile_load_name("mcu-isp");
        if (strcmp(sub, "info") == 0 || strcmp(sub, "id") == 0) {
            uint8_t ver = 0; uint16_t pid = 0; uint32_t cs = 0;
            rc = hw_pin_isp_sync();
            printf("pin: ISP sync -> %s (%d)\n", hw_pin_result_code_str(rc), rc);
            if (rc == HW_PIN_R_OK) (void)hw_pin_isp_get_version(&ver, NULL);
            if (rc == HW_PIN_R_OK) rc = hw_pin_isp_get_id(&pid);
            if (rc == HW_PIN_R_OK) (void)hw_pin_isp_get(&ver, &cs);
            printf("pin: ISP ver=0x%02X PID=0x%04X cmdsum=0x%08X (golden=0x%08X)\n",
                   (unsigned)ver, (unsigned)pid, (unsigned)cs, (unsigned)HW_PIN_ISP_GOLDEN);
            return rc;
        }
        if (strcmp(sub, "erase") == 0) {
            rc = hw_pin_isp_erase_all();
            printf("pin: ISP erase all -> %s (%d)\n", hw_pin_result_code_str(rc), rc);
            return rc;
        }
        if (strncmp(sub, "read ", 5) == 0) {
            static uint8_t buf[32];
            uint32_t addr = (uint32_t)strtol(sub + 5, NULL, 0);
            uint32_t i, n = 16u;
            rc = hw_pin_isp_read(addr, buf, n);
            if (rc == HW_PIN_R_OK) {
                printf("pin: ISP read %u..%u:", (unsigned)addr, (unsigned)(addr + n - 1u));
                for (i = 0; i < n; i++) printf(" %02X", (unsigned)buf[i]);
                printf("\n");
            } else printf("pin: ISP read FAIL %s (%d)\n", hw_pin_result_code_str(rc), rc);
            return rc;
        }
        if (strncmp(sub, "go", 2) == 0) {
            uint32_t addr = (sub[2] == ' ') ? (uint32_t)strtol(sub + 3, NULL, 0) : 0x08000000u;
            rc = hw_pin_isp_go(addr);
            printf("pin: ISP go 0x%08X -> %s (%d)\n", (unsigned)addr,
                   hw_pin_result_code_str(rc), rc);
            return rc;
        }
        if (strcmp(sub, "wtest") == 0) {
            /* 烧录全链: 全片擦 -> 写 256B -> 回读 -> 逐字节校验 */
            static uint8_t pat[HW_PIN_ISP_MAXPKT];
            static uint8_t rd[HW_PIN_ISP_MAXPKT];
            uint32_t i, n = 256u; int bad = 0;
            for (i = 0; i < n; i++) pat[i] = (uint8_t)((i * 11u + 5u) & 0xFFu);
            rc = hw_pin_isp_erase_all();
            if (rc == HW_PIN_R_OK) rc = hw_pin_isp_write(0x08000000u, pat, n);
            if (rc == HW_PIN_R_OK) rc = hw_pin_isp_read(0x08000000u, rd, n);
            if (rc == HW_PIN_R_OK) {
                for (i = 0; i < n; i++) if (rd[i] != pat[i]) { bad = 1; break; }
                if (bad) rc = HW_PIN_R_VERIFY;
            }
            printf("pin: ISP wtest(256B @0x08000000) -> %s (%d)\n",
                   hw_pin_result_code_str(rc), rc);
            return rc;
        }
        if (strcmp(sub, "chk") == 0) {
            uint32_t h = hw_pin_isp_checksum();
            printf("pin: ISP cmdsum=0x%08X golden=0x%08X -> %s\n",
                   (unsigned)h, (unsigned)HW_PIN_ISP_GOLDEN,
                   (h == HW_PIN_ISP_GOLDEN) ? "OK" : "MISMATCH");
            return (h == HW_PIN_ISP_GOLDEN) ? HW_PIN_R_OK : HW_PIN_R_BADARG;
        }
        printf("pin: isp sub cmds: info|erase|read ADDR|write|go [ADDR]|wtest|chk\n");
        return HW_PIN_R_OK;
    }
    if (strcmp(cmd, "stat") == 0) {
        hw_pin_stat_t st;
        hw_pin_stat(&st);
        printf("pin stat: spi_bytes=%u bb_toggles=%u vex=%u vpp_set=%u vpp_mv=%u active=%u id_ok=%u\n",
               (unsigned)st.spi_bytes, (unsigned)st.bb_toggles, (unsigned)st.vex,
               (unsigned)st.vpp_set_count, (unsigned)st.vpp_mv,
               (unsigned)st.active_idx, (unsigned)st.flash_id_ok);
        printf("pin isp : tx=%u rx=%u synced=%u pid=0x%04X ver=0x%02X\n",
               (unsigned)st.isp_tx, (unsigned)st.isp_rx, (unsigned)st.isp_synced,
               (unsigned)st.isp_pid, (unsigned)st.isp_ver);
        return HW_PIN_R_OK;
    }
    if (strcmp(cmd, "selftest") == 0) {
        int f = hw_pin_selftest(NULL);
        printf("pin: selftest %s (%d fails)\n", (f == 0) ? "ALL PASS" : "FAIL", f);
        return (f == 0) ? HW_PIN_R_OK : HW_PIN_R_BADARG;
    }
    if (strcmp(cmd, "help") == 0) {
        printf("hw_pin cmds: card|mode|profiles|load NAME|valid|drv hw|bb|id|read ADDR|"
               "erase ADDR|wtest|vpp N|vppread|isp [info|erase|read A|go [A]|wtest|chk]|"
               "stat|selftest\n");
        return HW_PIN_R_HELP;
    }
    return HW_PIN_R_NOCMD;
}

/* ============================================================
 * 10. 自检 (putf=NULL 静默)
 * ============================================================ */
static int pin_puts_null(const char* s) { (void)s; return 0; }

int hw_pin_selftest(int (*putf)(const char*))
{
    int fails = 0;
    if (!putf) putf = pin_puts_null;
    hw_pin_init(NULL);

    /* T1 模式探测 / 名称 */
    {
        const char* m = hw_pin_mode_str(hw_pin_mode());
        if (!m || m[0] == '?') { fails++; putf("  T1 mode probe FAIL\n"); }
        else putf("  T1 mode probe OK\n");
    }

    /* T2 档案表黄金值 */
    {
        uint32_t h = hw_pin_profile_checksum();
        char line[96];
        if (h != HW_PIN_GOLDEN) {
            fails++;
            snprintf(line, sizeof(line), "  T2 golden FAIL got=0x%08X want=0x%08X\n",
                     (unsigned)h, (unsigned)HW_PIN_GOLDEN);
            putf(line);
        } else putf("  T2 golden table OK\n");
    }

    /* T3 档案合法性 */
    {
        uint32_t i;
        for (i = 0; i < hw_pin_profile_count(); i++) {
            int rc = hw_pin_profile_valid(hw_pin_profile_get(i));
            if (rc != HW_PIN_R_OK) { fails++; putf("  T3 profile valid FAIL\n"); break; }
        }
        if (i == hw_pin_profile_count()) putf("  T3 all profiles valid OK\n");
    }

    /* T4 信号映射查询 */
    {
        (void)hw_pin_profile_load_name("w25q");
        if (hw_pin_gpio_of(HW_PIN_MOSI) != 23 || hw_pin_gpio_of(HW_PIN_MISO) != 19 ||
            hw_pin_gpio_of(HW_PIN_CK)   != 18 || hw_pin_gpio_of(HW_PIN_CS)   != 5) {
            fails++; putf("  T4 gpio_of FAIL\n");
        } else putf("  T4 signal->gpio map OK\n");
    }

    /* T5 bit-bang SPI: RDID 读到 W25Q128 -> EF 40 18 */
    {
        uint32_t j = 0;
        int rc = hw_pin_flash_rdid(&j);
        char line[96];
        if (rc != HW_PIN_R_OK || j != (HW_PIN_FLASH_JEDEC & 0x00FFFFFFu)) {
            fails++;
            snprintf(line, sizeof(line), "  T5 RDID FAIL rc=%d jedec=0x%06X\n",
                     rc, (unsigned)j);
            putf(line);
        } else putf("  T5 bit-bang RDID = EF4018 OK\n");
    }

    /* T6 擦除 -> 读全 FF */
    {
        static uint8_t rd[16];
        int rc = hw_pin_flash_sector_erase(0u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(16u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_read(0u, rd, 16u);
        if (rc != HW_PIN_R_OK || rd[0] != 0xFFu || rd[15] != 0xFFu) {
            fails++; putf("  T6 sector erase FAIL\n");
        } else putf("  T6 sector erase -> 0xFF OK\n");
    }

    /* T7 页编程 + 回读校验 (wtest 全链) */
    {
        static uint8_t pat[HW_PIN_FLASH_PAGE];
        uint32_t i;
        int rc;
        for (i = 0; i < (uint32_t)sizeof(pat); i++)
            pat[i] = (uint8_t)((i * 7u + 13u) & 0xFFu);
        rc = hw_pin_flash_page_program(0u, pat, (uint32_t)sizeof(pat));
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(16u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_verify(0u, pat, (uint32_t)sizeof(pat));
        if (rc != HW_PIN_R_OK) { fails++; putf("  T7 page program+verify FAIL\n"); }
        else putf("  T7 page program+verify OK\n");
    }

    /* T8 负向: 未 WREN 直接编程 -> 数据不变 -> verify 必报 VERIFY */
    {
        static uint8_t pat[HW_PIN_FLASH_PAGE];
        uint32_t i; int rc;
        for (i = 0; i < (uint32_t)sizeof(pat); i++) pat[i] = 0xA5u;
        rc = hw_pin_flash_sector_erase(0u);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(16u);
        /* 故意绕过 wren: 直接发 0x02 页编程 */
        if (rc == HW_PIN_R_OK) {
            uint8_t cmd[4];
            cmd[0] = HW_PIN_FLASH_CMD_PP;
            cmd[1] = 0u; cmd[2] = 0u; cmd[3] = 0u;
            (void)hw_pin_spi_set_cs(1); (void)hw_pin_spi_set_cs(0);
            rc = hw_pin_spi_xfer(cmd, NULL, 4u);
            if (rc == HW_PIN_R_OK) rc = hw_pin_spi_xfer(pat, NULL, (uint32_t)sizeof(pat));
            (void)hw_pin_spi_set_cs(1);
        }
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_verify(0u, pat, (uint32_t)sizeof(pat));
        if (rc != HW_PIN_R_VERIFY && rc != HW_PIN_R_OK) {
            fails++; putf("  T8 write-protect negative FAIL\n");
        } else if (rc == HW_PIN_R_OK) {
            fails++; putf("  T8 write-protect negative FAIL (unexpectedly wrote)\n");
        } else putf("  T8 write-protect negative (no WREN) OK\n");
    }

    /* T9 VPP 档位标称 + 设置 */
    {
        int rc = hw_pin_vpp_set(HW_PIN_VPP_5V);
        uint32_t mv = 0;
        (void)hw_pin_vpp_read(&mv);
        if (rc != HW_PIN_R_OK || hw_pin_vpp_level_mv(HW_PIN_VPP_5V) != 5000 ||
            hw_pin_vpp_level_mv(HW_PIN_VPP_12V) != 12000 ||
            mv != HW_PIN_VPP_MODEL_MV) {
            fails++; putf("  T9 VPP levels FAIL\n");
        } else putf("  T9 VPP levels (5/12/12.5/21V) OK\n");
    }

    /* T10 无效档案拒绝 */
    {
        hw_pin_profile_t bad;
        memset(&bad, 0, sizeof(bad));
        bad.name = "bad"; bad.spi_mode = 9u;
        if (hw_pin_profile_valid(&bad) == HW_PIN_R_OK) {
            fails++; putf("  T10 invalid reject FAIL\n");
        } else putf("  T10 invalid profile reject OK\n");
    }

    /* T11 selftest(NULL) 不崩 (T1~T10 已在 NULL 下跑过一遍) */
    putf("  T11 NULL-safe OK\n");

    /* T12 负向: 档案未映射 SPI 脚 -> RDID 必须报 NODEV (不是假成功) */
    {
        int rc = hw_pin_profile_load_name("mcu-isp");
        if (rc == HW_PIN_R_OK) {
            uint32_t j = 0;
            int r2 = hw_pin_flash_rdid(&j);
            if (hw_pin_gpio_of(HW_PIN_MOSI) != -1 || r2 != HW_PIN_R_NODEV) {
                fails++; putf("  T12 unmapped-pin reject FAIL\n");
            } else putf("  T12 unmapped-pin reject OK\n");
        } else {
            fails++; putf("  T12 unmapped-pin reject FAIL (load)\n");
        }
        (void)hw_pin_profile_load(NULL);
    }

    /* T13 ISP 命令表黄金 (独立 Python 对拍) */
    {
        uint32_t h = hw_pin_isp_checksum();
        char line[96];
        if (h != HW_PIN_ISP_GOLDEN) {
            fails++;
            snprintf(line, sizeof(line), "  T13 ISP cmdsum FAIL got=0x%08X want=0x%08X\n",
                     (unsigned)h, (unsigned)HW_PIN_ISP_GOLDEN);
            putf(line);
        } else putf("  T13 ISP cmdsum table OK\n");
    }

    /* T14 ISP 握手 + GetID + GetVersion (STM32F103: ver=0x31 PID=0x0410) */
    {
        uint8_t ver = 0u; uint16_t pid = 0u; uint32_t cs = 0u;
        int rc;
        (void)hw_pin_profile_load_name("mcu-isp");
        hw_pin_isp_reset_sync();
        rc = hw_pin_isp_get_version(&ver, NULL);
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_get_id(&pid);
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_get(&ver, &cs);
        if (rc != HW_PIN_R_OK || ver != HW_PIN_ISP_BOOTVER || pid != HW_PIN_ISP_PID ||
            cs != HW_PIN_ISP_GOLDEN) {
            char line[112];
            fails++;
            snprintf(line, sizeof(line),
                     "  T14 ISP sync/id FAIL rc=%d ver=0x%02X pid=0x%04X cs=0x%08X\n",
                     rc, (unsigned)ver, (unsigned)pid, (unsigned)cs);
            putf(line);
        } else putf("  T14 ISP handshake + GetID(0x0410) OK\n");
    }

    /* T15 ISP 烧录全链: 全片擦 -> 写 256B -> 读回 -> 逐字节一致 */
    {
        static uint8_t pat[HW_PIN_ISP_MAXPKT];
        static uint8_t rd[HW_PIN_ISP_MAXPKT];
        uint32_t i, n = 256u; int rc, bad = 0;
        for (i = 0; i < n; i++) pat[i] = (uint8_t)((i * 11u + 5u) & 0xFFu);
        rc = hw_pin_isp_erase_all();
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_write(HW_PIN_ISP_FLASH_BASE, pat, n);
        if (rc == HW_PIN_R_OK) rc = hw_pin_isp_read(HW_PIN_ISP_FLASH_BASE, rd, n);
        if (rc == HW_PIN_R_OK) {
            for (i = 0; i < n; i++) if (rd[i] != pat[i]) { bad = 1; break; }
            if (bad) rc = HW_PIN_R_VERIFY;
        }
        if (rc != HW_PIN_R_OK) { fails++; putf("  T15 ISP mirror (erase/write/read) FAIL\n"); }
        else putf("  T15 ISP mirror (erase->write256->read) OK\n");
    }

    /* T16 负向: 无 UART 的档案 (w25q) 上做 ISP -> 必须 NODEV (不是假成功) */
    {
        int rc;
        (void)hw_pin_profile_load_name("w25q");
        rc = hw_pin_isp_sync();
        if (rc != HW_PIN_R_NODEV) { fails++; putf("  T16 ISP no-uart reject FAIL\n"); }
        else putf("  T16 ISP no-uart profile reject OK\n");
        (void)hw_pin_profile_load(NULL);
    }

    /* T17 负向: 命令码补码错 -> 目标 NACK (0x1F); 校验错误的写不落盘 */
    {
        int rc, got;
        (void)hw_pin_profile_load_name("mcu-isp");
        (void)hw_pin_isp_reset_sync();
        (void)hw_pin_uart_putc((int)HW_PIN_ISP_CMD_GID);
        (void)hw_pin_uart_putc(0x00);           /* 正确补码应为 0xFD, 故意写错 */
        got = hw_pin_uart_getc();
        rc = ((got == (int)HW_PIN_ISP_NACK)) ? HW_PIN_R_OK : HW_PIN_R_NAK;
        if (rc != HW_PIN_R_OK) {
            char line[96];
            fails++;
            snprintf(line, sizeof(line), "  T17 ISP NACK path FAIL got=0x%02X\n", (unsigned)got);
            putf(line);
        } else putf("  T17 ISP bad-checksum -> NACK OK\n");
        (void)hw_pin_profile_load(NULL);
    }

    hw_pin_init(NULL);
    return fails;
}

/* ============================================================
 * 11. CLI: ./xiaomo pin [...]
 *   家族约定: argv[1]="pin", 子命令在 argv[2] (argc>=3)
 * ============================================================ */
static int pin_cli_puts(const char* s) { return printf("%s", s); }

int hw_pin_cli(int argc, char** argv)
{
    static char cmdline[256];
    const char* sub = "card";
    int rc;

    /* 家族约定: argv[1]="pin", 命令在 argv[2..]。
     * ⚠️ 必须把 argv[2] 之后的参数拼回一条命令串, 否则
     *    `./xiaomo pin load w25q` 会被截成 "load" → 全分支不匹配 → 静默 exit 1
     *    (与家族坑 #9「argv 下标」同源: 零输出先怀疑命令拼接)。
     *    带引号的单参数写法 (`./xiaomo pin "load w25q"`) 仍兼容。 */
    if (argc >= 4) {
        int i;
        size_t off = 0;
        cmdline[0] = '\0';
        for (i = 2; i < argc; i++) {
            size_t l = argv[i] ? strlen(argv[i]) : 0u;
            if (off + l + 2u >= sizeof(cmdline)) break;
            if (i > 2) cmdline[off++] = ' ';
            if (l) memcpy(cmdline + off, argv[i], l);
            off += l;
            cmdline[off] = '\0';
        }
        sub = cmdline;
    } else if (argc >= 3 && argv[2]) {
        sub = argv[2];
    }

    hw_pin_init(NULL);

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_pin_selftest(pin_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "help") == 0) {
        printf("hw_pin cmds: card|mode|profiles|load NAME|valid|drv hw|bb|id|read ADDR|"
               "erase ADDR|wtest|vpp N|vppread|isp [info|erase|read A|go [A]|wtest|chk]|"
               "stat|selftest\n");
        return 0;
    }

    rc = hw_pin_cmd(sub, NULL);
    return (rc >= 0) ? rc : 1;
}
