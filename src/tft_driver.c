/*
 * tft_driver.c — xiaomo MMU 驱动的 10-Pin TFT 显示屏驱动
 *
 * 设计原则:
 *   1. Ring0 控制硬件 (SPI/GPIO)，Ring3 通过帧缓冲+SYSCALL 访问
 *   2. MMU 保护帧缓冲: Ring3 可写但不能访问硬件寄存器
 *   3. 支持 Linux (FFI→/dev/spidev) 和 裸机 (MMIO→SPI寄存器) 双模式
 *
 * 接线对照 (10-Pin TFT → LonganPi 3H GPIO):
 *   TFT VCC  → 3.3V (pin 1, 17)
 *   TFT GND  → GND  (pin 6, 9, 14, ...)
 *   TFT CS   → SPI1_CS1  (PH9)       (根据 DTS: cs1)
 *   TFT RST  → PG13
 *   TFT DC   → PG11
 *   TFT MOSI → SPI1_MOSI (PH7)
 *   TFT SCK  → SPI1_CLK  (PH6)
 *   TFT LED  → 3.3V 或 PWM 控制的 GPIO
 *   TFT MISO → SPI1_MISO (PH8) — 读 LCD 状态用
 */

#include "tft_driver.h"
#include "mmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>   /* Linux SPI userspace API */
#include <sys/mman.h>
#include <time.h>

/* ===================== 内部辅助 ===================== */

static void delay_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ===================== 控制器初始化序列 ===================== */

/* ST7735 1.8" 128×160 初始化序列 */
static const uint8_t init_st7735[] = {
    /* SWRESET */     0x01, 0x00,
    /* SLPOUT */      0x11, 0x00,
    /* FRMCTR1 */     0xB1, 0x03, 0x05, 0x3C, 0x3C, 0x1B,
    /* FRMCTR2 */     0xB2, 0x03, 0x05, 0x3C, 0x3C,
    /* FRMCTR3 */     0xB3, 0x03, 0x05, 0x3C, 0x3C, 0x05, 0x3C, 0x3C,
    /* INVCTR */      0xB4, 0x03,
    /* PWCTR1 */      0xC0, 0x62, 0x02, 0x04,
    /* PWCTR2 */      0xC1, 0xC0,
    /* PWCTR3 */      0xC2, 0x0D, 0x00,
    /* PWCTR4 */      0xC3, 0x8D, 0x6A,
    /* PWCTR5 */      0xC4, 0x8D, 0xEE,
    /* VMCTR1 */      0xC5, 0x0E,
    /* GMCTRP1 */     0xE0, 0x10, 0x0E, 0x02, 0x03, 0x0E, 0x05, 0x02,
                        0x11, 0x0D, 0x11, 0x3C, 0x36, 0x10, 0x11, 0x18,
    /* GMCTRN1 */     0xE1, 0x10, 0x0E, 0x02, 0x03, 0x0E, 0x05, 0x02,
                        0x11, 0x0D, 0x11, 0x3C, 0x36, 0x10, 0x11, 0x18,
    /* MADCTL */      0x36, 0x00,
    /* COLMOD */      0x3A, 0x05,     /* RGB565 */
    /* DISPON */      0x29, 0x00,
    0x00  /* 终止符 */
};

/* ST7789 1.14" 135×240 / 170×320 初始化序列 */
static const uint8_t init_st7789[] = {
    /* SWRESET */     0x01, 0x00,
    /* SLPOUT */      0x11, 0x00,     delay_ms(120),
    /* COLMOD */      0x3A, 0x05,     /* RGB565 */
    /* PORCTRL */     0xB2, 0x0B, 0x0B, 0x00, 0x33, 0x33,
    /* GCTRL */       0xB7, 0x75,
    /* VCOMS */       0xBB, 0x28,
    /* LCMCTRL */     0xC0, 0x2C,
    /* VDVVRHEN */    0xC2, 0x01, 0xFF,
    /* VRHS */        0xC3, 0x1F,
    /* VDVS */        0xC4, 0x13,
    /* VCMOFSET */    0xC5, 0x28,
    /* PWCTRL1 */     0xD0, 0xA4, 0xA1,
    /* PVGAMMA */     0xE0, 0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B,
                        0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32,
    /* NVGAMMA */     0xE1, 0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B,
                        0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37,
    /* MADCTL */      0x36, 0x00,
    /* INVOFF */      0x20, 0x00,
    /* DISPON */      0x29, 0x00,
    0x00
};

/* ===================== 底层 SPI ===================== */

int tft_spi_open_linux(TftDriver* drv, const char* dev_path) {
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) { perror("spi open"); return -1; }

    /* 配置 SPI 模式: Mode 3 (CPOL=1, CPHA=1) —— ST77xx 常用 */
    uint8_t mode = drv->spi_mode;
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) { perror("spi mode"); close(fd); return -1; }

    /* 每字 8 bit */
    uint8_t bits = drv->spi_bits;
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) { perror("spi bits"); close(fd); return -1; }

    /* 速度 */
    uint32_t speed = drv->spi_speed_hz;
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) { perror("spi speed"); close(fd); return -1; }

    drv->spi_fd = fd;
    return 0;
}

void tft_spi_close_linux(TftDriver* drv) {
    if (drv->spi_fd >= 0) { close(drv->spi_fd); drv->spi_fd = -1; }
}

int tft_spi_transfer_linux(TftDriver* drv,
                           const uint8_t* tx, uint8_t* rx, int len) {
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = (uint32_t)len,
        .speed_hz = drv->spi_speed_hz,
        .bits_per_word = drv->spi_bits,
        .cs_change = 0,
    };
    return ioctl(drv->spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

/* ===================== GPIO (Linux) ===================== */
/*
 * 通过 /sys/class/gpio 或 /dev/gpiochip*
 * 这里用 /sys/class/gpio 简化
 */
static int gpio_export(uint16_t gpio) {
    char buf[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd < 0) return -1;
    int n = snprintf(buf, sizeof(buf), "%d", gpio);
    write(fd, buf, n); close(fd);
    return 0;
}

static void gpio_set_dir(uint16_t gpio, const char* dir) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, dir, strlen(dir)); close(fd); }
}

static void gpio_write(uint16_t gpio, int val) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, val ? "1" : "0", 1); close(fd); }
}

/* 计算 H616 的 GPIO 编号: port=PG→6, pin=11 → 6*32+11=203 */
#define GPIO_NUM(port, pin)  ((port)*32 + (pin))

/* ===================== TFT 驱动核心 API ===================== */

/* 计算帧缓冲大小 (RGB565: 2 bytes/pixel) */
#define FB_SIZE(w, h)  ((w)*(h)*2)

int tft_init(TftDriver* drv, TftController ctl,
             uint16_t w, uint16_t h) {
    memset(drv, 0, sizeof(TftDriver));
    drv->controller = ctl;
    drv->width  = w;
    drv->height = h;
    drv->bpp    = 16;  /* RGB565 */
    drv->spi_speed_hz = 40000000;  /* 40MHz default */
    drv->spi_mode     = SPI_MODE_3;  /* CPOL=1, CPHA=1 */
    drv->spi_bits     = 8;
    drv->brightness   = 255;
    drv->spi_fd       = -1;
    drv->pin_led      = 0;

    /* 默认: LonganPi 3H 的接线 */
    drv->pin_cs  = GPIO_NUM(7, 9);   /* PH9: SPI1_CS1 */
    drv->pin_dc  = GPIO_NUM(6, 11);  /* PG11: DC */
    drv->pin_rst = GPIO_NUM(6, 13);  /* PG13: RST */
    drv->pin_led = GPIO_NUM(6, 12);  /* PG12: LED/BL (可选) */

    /* 分配帧缓冲 */
    uint32_t fb_size = FB_SIZE(w, h);
    uint16_t* fb = (uint16_t*)aligned_alloc(16, fb_size);
    if (!fb) return -1;
    memset(fb, 0, fb_size);
    /* 驱动控制块记录 fb_base — Ring3 通过 MMU 段访问 */
    /* 这里用 drv 自身存不了 fb 指针，需要放在外部，见 tft_get_framebuffer */

    return 0;
}

uint16_t* tft_get_framebuffer(TftDriver* drv) {
    /* 帧缓冲地址应在 init 时分配后存于某处 */
    /* 简化: 用 static 持有，实际应挂在 drv 扩展域 */
    (void)drv;
    static uint16_t* fb = NULL;
    if (!fb) {
        uint32_t sz = FB_SIZE(240, 320); /* max */
        fb = (uint16_t*)aligned_alloc(32, sz);
        memset(fb, 0, sz);
    }
    return fb;
}

/* ===================== MMU 段管理 ===================== */

void tft_mmu_map_fb(TftDriver* drv, void* mmu_ptr, int seg_idx) {
    Mmu* mmu = (Mmu*)mmu_ptr;
    uint16_t* fb = tft_get_framebuffer(drv);
    uint32_t fb_sz = FB_SIZE(drv->width, drv->height);

    /* 段: Ring3 可读可写, dpl=3 */
    mmu_set_segment(mmu, seg_idx,
        SEG_DATA | SEG_WRITE,
        (uint16_t)(uintptr_t)fb,   /* base */
        fb_sz,                     /* limit */
        SEG_READ | SEG_WRITE,      /* perm */
        3);                        /* dpl=3 → Ring3 可访问 */
    drv->seg_fb = seg_idx;
}

void tft_mmu_unmap_fb(TftDriver* drv, void* mmu_ptr) {
    Mmu* mmu = (Mmu*)mmu_ptr;
    if (drv->seg_fb >= 0) {
        /* 清除段 → Ring3 不能再访问帧缓冲 */
        mmu_set_segment(mmu, drv->seg_fb, SEG_NULL, 0, 0, 0, 0);
        drv->seg_fb = -1;
    }
}

/* ===================== 刷屏 (帧缓冲 → LCD) ===================== */

int tft_flush(TftDriver* drv) {
    uint16_t* fb = tft_get_framebuffer(drv);
    if (!fb) return -1;

    /* 设置全屏窗口 */
    tft_set_window(drv, 0, 0, drv->width - 1, drv->height - 1);

    /* 写入像素数据 (RAMWR) */
    tft_write_cmd(drv, 0x2C);  /* RAMWR */

    /* 通过 SPI 推数据 */
    /* RGB565: 2 bytes per pixel, MSB first */
    uint32_t total_bytes = FB_SIZE(drv->width, drv->height);
    tft_spi_transfer_linux(drv, (const uint8_t*)fb, NULL, total_bytes);

    return 0;
}

/* ===================== 命令/数据写入 ===================== */

void tft_write_cmd(TftDriver* drv, uint8_t cmd) {
    gpio_write(drv->pin_dc, 0);   /* DC=0: 命令 */
    gpio_write(drv->pin_cs, 0);   /* CS=0: 片选 */
    tft_spi_transfer_linux(drv, &cmd, NULL, 1);
    gpio_write(drv->pin_cs, 1);   /* CS=1: 释放 */
}

void tft_write_data(TftDriver* drv, const uint8_t* data, int len) {
    gpio_write(drv->pin_dc, 1);   /* DC=1: 数据 */
    gpio_write(drv->pin_cs, 0);
    tft_spi_transfer_linux(drv, data, NULL, len);
    gpio_write(drv->pin_cs, 1);
}

void tft_write_data16(TftDriver* drv, const uint16_t* data, int len) {
    /* 转成 uint8_t 数组 (大端: MSB first) */
    /* 对于 40MHz SPI, 更高效是直接发 u16 大端 */
    gpio_write(drv->pin_dc, 1);
    gpio_write(drv->pin_cs, 0);
    /* spi_transfer 按字节发送, 保证大端顺序 */
    for (int i = 0; i < len; i++) {
        uint8_t buf[2] = { (uint8_t)(data[i] >> 8), (uint8_t)(data[i] & 0xFF) };
        tft_spi_transfer_linux(drv, buf, NULL, 2);
    }
    gpio_write(drv->pin_cs, 1);
}

/* ===================== 窗口/区域设置 ===================== */

void tft_set_window(TftDriver* drv, uint16_t x0, uint16_t y0,
                    uint16_t x1, uint16_t y1) {
    uint8_t data[4];

    /* CASET (Column Address Set) */
    tft_write_cmd(drv, 0x2A);
    data[0] = x0 >> 8; data[1] = x0 & 0xFF;
    data[2] = x1 >> 8; data[3] = x1 & 0xFF;
    tft_write_data(drv, data, 4);

    /* RASET (Row Address Set) */
    tft_write_cmd(drv, 0x2B);
    data[0] = y0 >> 8; data[1] = y0 & 0xFF;
    data[2] = y1 >> 8; data[3] = y1 & 0xFF;
    tft_write_data(drv, data, 4);
}

/* ===================== 基本操作 ===================== */

void tft_reset(TftDriver* drv) {
    gpio_write(drv->pin_rst, 0);
    delay_ms(10);
    gpio_write(drv->pin_rst, 1);
    delay_ms(120);
}

void tft_set_brightness(TftDriver* drv, uint8_t level) {
    drv->brightness = level;
    /* 如果有 PWM 背光引脚, 通过 PWM 调亮度 */
    if (drv->pin_led) {
        /* 简化: 50% 以上 = 开, 否则 = 关 */
        gpio_write(drv->pin_led, (level > 0) ? 1 : 0);
    }
}

void tft_display_on(TftDriver* drv) {
    tft_write_cmd(drv, 0x29);  /* DISPON */
    drv->display_on = true;
}

void tft_display_off(TftDriver* drv) {
    tft_write_cmd(drv, 0x28);  /* DISPOFF */
    drv->display_on = false;
}

void tft_clear(TftDriver* drv, uint16_t color) {
    uint16_t* fb = tft_get_framebuffer(drv);
    uint32_t n = (uint32_t)drv->width * drv->height;
    for (uint32_t i = 0; i < n; i++) fb[i] = color;
    tft_flush(drv);
}

void tft_deinit(TftDriver* drv) {
    tft_display_off(drv);
    tft_spi_close_linux(drv);
    drv->initialized = false;
}

/* ===================== 初始化序列执行 ===================== */

/*
 * 执行初始化序列:
 *   每字节流格式: cmd, n_data_bytes, data[0..n-1], ...
 *   遇到 0x00 终止
 */
static void run_init_seq(TftDriver* drv, const uint8_t* seq) {
    while (*seq) {
        uint8_t cmd = *seq++;
        int n = *seq++;  /* 数据字节数 */
        if (n > 0) {
            /* 带数据的命令: 发命令然后写数据 */
            tft_write_cmd(drv, cmd);
            tft_write_data(drv, seq, n);
            seq += n;
        } else {
            /* 纯命令 (无数据) */
            tft_write_cmd(drv, cmd);
        }
    }
}

/*
 * 完整初始化 (接线 → SPI → GPIO → LCD init)
 *
 * 使用示例 (在 Kickpi Linux 上):
 *
 *   TftDriver drv;
 *   tft_init(&drv, TFT_ST7789, 170, 320);
 *
 *   // 1. 导出 GPIO
 *   gpio_export(drv.pin_cs);  gpio_set_dir(drv.pin_cs, "out");
 *   gpio_export(drv.pin_dc);  gpio_set_dir(drv.pin_dc, "out");
 *   gpio_export(drv.pin_rst); gpio_set_dir(drv.pin_rst, "out");
 *   gpio_export(drv.pin_led); gpio_set_dir(drv.pin_led, "out");
 *
 *   // 2. 打开 SPI 设备
 *   tft_spi_open_linux(&drv, "/dev/spidev1.1");
 *
 *   // 3. 硬件复位
 *   tft_reset(&drv);
 *
 *   // 4. 执行初始化序列
 *   run_init_seq(&drv, init_st7789);
 *
 *   // 5. 设置 MMU 帧缓冲段 (Ring3 可访问)
 *   tft_mmu_map_fb(&drv, &mmu, 1);
 *
 *   // 6. 点亮
 *   tft_display_on(&drv);
 *   tft_set_brightness(&drv, 255);
 *   tft_clear(&drv, 0x0000);  // 黑屏
 *
 * 之后 Ring3 程序直接写帧缓冲, 调用 SYSCALL 刷屏。
 */
int tft_init_full(TftDriver* drv, TftController ctl,
                  uint16_t w, uint16_t h,
                  const char* spi_dev) {
    /* 1. 初始化驱动结构 */
    int rc = tft_init(drv, ctl, w, h);
    if (rc < 0) return rc;

    /* 2. 导出并配置 GPIO */
    gpio_export(drv->pin_cs);  gpio_set_dir(drv->pin_cs, "out");
    gpio_export(drv->pin_dc);  gpio_set_dir(drv->pin_dc, "out");
    gpio_export(drv->pin_rst); gpio_set_dir(drv->pin_rst, "out");
    gpio_export(drv->pin_led); gpio_set_dir(drv->pin_led, "out");
    gpio_write(drv->pin_cs,  1);  /* CS 默认高 */
    gpio_write(drv->pin_led, 1);  /* 背光默认开 */

    /* 3. 打开 SPI */
    rc = tft_spi_open_linux(drv, spi_dev);
    if (rc < 0) return rc;

    /* 4. 硬件复位 */
    tft_reset(drv);

    /* 5. 执行初始化序列 */
    switch (ctl) {
    case TFT_ST7735:
        run_init_seq(drv, init_st7735);
        break;
    case TFT_ST7789:
    case TFT_ST7789T3:
        run_init_seq(drv, init_st7789);
        break;
    default:
        run_init_seq(drv, init_st7789); /* fallback */
        break;
    }

    drv->initialized = true;

    /* 6. 显示打开 */
    tft_display_on(drv);
    tft_clear(drv, 0x0000);

    return 0;
}