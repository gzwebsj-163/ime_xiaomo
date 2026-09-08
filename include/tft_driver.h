/*
 * tft_driver.h — xiaomo MMU 驱动的 TFT 显示屏驱动
 *
 * 架构: Ring0 内核驱动 + Ring3 用户帧缓冲 + MMU 隔离
 *
 * 屏幕: 10-Pin SPI TFT (ST7735/ST7789)
 * 接线: VCC GND CS RESET DC MOSI SCK LED MISO NC
 *
 *          ┌──────────────────────┐
 * Ring 3   │  .mo Application     │
 *          │  → 直接写帧缓冲内存   │
 *          │  → SYSCALL 刷屏      │
 *          └────────┬─────────────┘
 *                   │ SYSCALL
 *          ┌────────┴─────────────┐
 * Ring 0   │  TFT Driver          │
 *          │  · SPI 控制器 MMIO   │
 *          │  · GPIO DC/RST/LED   │
 *          │  · 帧缓冲→SPI DMA    │
 *          │  · MMU 段管理        │
 *          └────────┬─────────────┘
 *                   │ hw_mmio_* / FFI
 *          ┌────────┴─────────────┐
 *          │  SPI1 / GPIO 硬件    │ ←→ ST7789/ST7735
 *          └──────────────────────┘
 *
 * MMU 段布局:
 *   Seg 0: 内核代码+数据 (Ring0, R+W+X, dpl=0)
 *   Seg 1: 帧缓冲 (Ring3, R+W,   dpl=3)  ← 用户应用画图
 *   Seg 2: 驱动控制块 (Ring0, R+W, dpl=0)
 *   Seg 3: SPI TX DMA 缓冲区 (Ring0, R+W, dpl=0)
 */
#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ========== 10-Pin TFT 标准定义 ========== */

/* 常见 10-Pin 定义 (按序号) */
#define TFT_PIN_VCC   1   /* 3.3V */
#define TFT_PIN_GND   2   /* GND */
#define TFT_PIN_CS    3   /* SPI 片选 */
#define TFT_PIN_RESET 4   /* 硬件复位 */
#define TFT_PIN_DC    5   /* 数据/命令选择 */
#define TFT_PIN_MOSI  6   /* SPI 主机出从机入 */
#define TFT_PIN_SCK   7   /* SPI 时钟 */
#define TFT_PIN_LED   8   /* 背光 */
#define TFT_PIN_MISO  9   /* SPI 主机入从机出 (可选) */
#define TFT_PIN_NC   10   /* 保留/触摸 */

/* ========== 控制器类型 ========== */
typedef enum {
    TFT_UNKNOWN = 0,
    TFT_ST7735,      /* 128×160, 80×160 (0.96") */
    TFT_ST7789,      /* 170×320, 135×240 (1.14") */
    TFT_ST7789T3,    /* LonganPi 3H 1.9寸 ST7789T3 variant */
    TFT_ILI9341,     /* 240×320 (2.8"/3.2") */
    TFT_SSD1351      /* 128×128 OLED */
} TftController;

/* ========== 驱动控制块 ========== */
typedef struct {
    /* === 屏幕参数 === */
    TftController  controller;   /* 控制器型号 */
    uint16_t       width;        /* 显示宽度 (px) */
    uint16_t       height;       /* 显示高度 (px) */
    uint8_t        bpp;          /* 每像素比特 (16=RGB565) */
    uint8_t        rotation;     /* 旋转角度 (0/90/180/270) */
    bool           bgr;          /* BGR 颜色顺序 */
    uint32_t       spi_speed_hz; /* SPI 时钟 (Hz) */
    int            te_pin;       /* TE 引脚 (-1=无) */

    /* === SPI 硬件接口 === */
    void*          spi_base;     /* SPI 控制器 MMIO 基地址 */
    int            spi_fd;       /* /dev/spidev* 文件描述符 (Linux) */
    uint8_t        spi_mode;     /* SPI 模式 (CPOL/CPHA) */
    uint8_t        spi_bits;     /* 每字位数 (8) */

    /* === GPIO === */
    uint16_t       pin_cs;       /* CS GPIO 编号 */
    uint16_t       pin_dc;       /* DC GPIO 编号 */
    uint16_t       pin_rst;      /* RESET GPIO 编号 */
    uint16_t       pin_led;      /* LED/背光 GPIO 编号 */

    /* === MMU 段号 === */
    int            seg_fb;       /* 帧缓冲段号 (Ring3 可访问) */
    int            seg_dma;      /* SPI DMA 段号 (仅 Ring0) */

    /* === 状态 === */
    bool           initialized;  /* 驱动已初始化 */
    bool           display_on;   /* 显示已开启 */
    uint8_t        brightness;   /* 背光亮度 (0-255) */

    /* === 初始化序列 (由具体控制器填充) === */
    const uint8_t* init_cmds;    /* 初始化命令序列 */
    int            init_cmd_len; /* 命令序列长度 */
} TftDriver;

/* ========== API (Ring0 → 驱动层) ========== */

/* 初始化驱动 — 设置 GPIO, SPI, MMU 段, 执行 LCD 初始化序列 */
int  tft_init(TftDriver* drv, TftController ctl,
              uint16_t w, uint16_t h);

/* 发送命令到 LCD */
void tft_write_cmd(TftDriver* drv, uint8_t cmd);

/* 发送数据到 LCD */
void tft_write_data(TftDriver* drv, const uint8_t* data, int len);

/* 发送 16-bit 数据 (RGB565 像素写入) */
void tft_write_data16(TftDriver* drv, const uint16_t* data, int len);

/* 帧缓冲 → LCD 刷屏 (MMU DMA 路径) */
int  tft_flush(TftDriver* drv);

/* 设置显示区域 (CASET/RASET) */
void tft_set_window(TftDriver* drv, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1);

/* 硬件复位 */
void tft_reset(TftDriver* drv);

/* 背光控制 */
void tft_set_brightness(TftDriver* drv, uint8_t level);

/* 显示开关 */
void tft_display_on(TftDriver* drv);
void tft_display_off(TftDriver* drv);

/* 清屏 */
void tft_clear(TftDriver* drv, uint16_t color);

/* 销毁驱动 (释放资源) */
void tft_deinit(TftDriver* drv);

/* ========== MMU 集成接口 ========== */

/*
 * MMU 段布局 (由 tft_init 配置):
 *
 * 段号 | 用途        | Ring | 权限  | dpl
 * ─────┼─────────────┼──────┼───────┼─────
 *  0   | 内核代码+数据 |  0  | R+W+X |  0
 *  1   | 帧缓冲       |  3  | R+W   |  3  ← 用户程序画图
 *  2   | 驱动控制块   |  0  | R+W   |  0
 *  3   | SPI DMA 缓冲  |  0  | R+W   |  0
 */

/* 获取帧缓冲基地址 (Ring3 通过此地址直接绘制) */
uint16_t* tft_get_framebuffer(TftDriver* drv);

/* 使帧缓冲对 Ring3 可见 (设置 MMU 段) */
void tft_mmu_map_fb(TftDriver* drv, void* mmu, int seg_idx);

/* 使帧缓冲对 Ring3 不可见 (刷屏完成后收回权限) */
void tft_mmu_unmap_fb(TftDriver* drv, void* mmu);

/* SYSCALL 编号定义 */
#define TFT_SYSCALL_FLUSH    1   /* 刷屏 */
#define TFT_SYSCALL_CLEAR    2   /* 清屏 */
#define TFT_SYSCALL_SET_BL   3   /* 背光 */
#define TFT_SYSCALL_ON       4   /* 显示开 */
#define TFT_SYSCALL_OFF      5   /* 显示关 */
#define TFT_SYSCALL_SET_WIN  6   /* 设窗口 */

/*
 * SYSCALL 调用约定 (Ring3→Ring0):
 *   R0 = TFT_SYSCALL_xxx  (功能号)
 *   R1 = 参数1 (如颜色值/亮度/窗口坐标)
 *   R2 = 参数2
 *   ret = 0 成功 / -1 失败
 */

/* ========== 硬件抽象: SPI GPIO 操作 ========== */

/* Linux (FFI): 通过 /dev/spidev 访问 */
int  tft_spi_open_linux(TftDriver* drv, const char* dev_path);
void tft_spi_close_linux(TftDriver* drv);
int  tft_spi_transfer_linux(TftDriver* drv,
                            const uint8_t* tx, uint8_t* rx, int len);

/* 裸机 (MMIO): 直接操作 SPI 控制器寄存器 */
int  tft_spi_init_mmio(TftDriver* drv, uint64_t spi_phys_base);
void tft_spi_xfer_mmio(TftDriver* drv,
                       const uint8_t* tx, uint8_t* rx, int len);

/* GPIO 操作 (MMIO: 直接操作 GPIO 寄存器) */
void tft_gpio_write(TftDriver* drv, uint16_t pin, int val);
void tft_gpio_set_mode(TftDriver* drv, uint16_t pin, int mode);

#endif /* TFT_DRIVER_H */