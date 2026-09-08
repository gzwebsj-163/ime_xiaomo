/*
 * xiaomo - 硬件直访模块 头文件
 *
 * 提供汇编级别的硬件访问能力:
 *   IO 端口 / PCI 配置空间 / MMIO / MSR / 设备枚举 / UART
 */

#ifndef XIAOMO_HW_DIRECT_H
#define XIAOMO_HW_DIRECT_H

#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#include <termios.h>  /* UART */
/* macOS 专属内存统计接口 - 其他平台(Android Termux/Linux)跳过 */
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 错误处理 ========== */
const char* hw_get_error(void);

/* ========== IO 端口 (x86 in/out) ========== */
int hw_inb(uint16_t port, uint8_t* val);
int hw_outb(uint16_t port, uint8_t val);
int hw_inl(uint16_t port, uint32_t* val);
int hw_outl(uint16_t port, uint32_t val);

/* ========== PCI 配置空间 ========== */
int hw_pci_read(uint8_t bus, uint8_t dev, uint8_t func,
                uint32_t offset, uint32_t* val, int width);
int hw_pci_write(uint8_t bus, uint8_t dev, uint8_t func,
                 uint32_t offset, uint32_t val, int width);

/* ========== MMIO 内存映射 IO ========== */
int hw_mmio_map(uint64_t phys_addr, uint64_t size);
int hw_mmio_unmap(void);
int hw_mmio_read(uint64_t offset, uint64_t* val, int width);
int hw_mmio_write(uint64_t offset, uint64_t val, int width);

/* ========== MSR 寄存器 ========== */
int hw_msr_read(uint32_t msr_addr, uint64_t* val);
int hw_msr_write(uint32_t msr_addr, uint64_t val);

/* ========== 设备枚举 ========== */
int hw_pci_enumerate(char* buf, int buflen);
int hw_usb_enumerate(char* buf, int buflen);
int hw_serial_enumerate(char* buf, int buflen);

/* ========== CPU 信息 ========== */
int hw_cpu_info(char* buf, int buflen);

/* ========== UART 串口 ========== */
int hw_uart_open(const char* path, int baud);
int hw_uart_close(int handle);
int hw_uart_read(int handle, uint8_t* buf, int maxlen);
int hw_uart_write(int handle, const uint8_t* buf, int len);

/* ========== 系统信息 ========== */
int hw_phys_mem_info(uint64_t* total, uint64_t* free_bytes);

/* ========== SPI (Linux spidev) ========== */
int hw_spi_open(const char* dev_path, int mode, int bits, int speed);
int hw_spi_close(int handle);
int hw_spi_transfer(int handle, const uint8_t* tx, uint8_t* rx, int len);

/* ========== I2C (Linux /dev/i2c-N ioctl, 2026-09-07) ========== */
int hw_i2c_open(const char* dev_path, uint16_t addr);   /* -> handle(>=0) / -1 */
int hw_i2c_close(int handle);
int hw_i2c_write(int handle, const uint8_t* buf, int len);
int hw_i2c_read(int handle, uint8_t* buf, int maxlen);
int hw_i2c_xfer(int handle, const uint8_t* tx, int txlen,
                uint8_t* rx, int rxmaxlen);              /* 写-读组合 (I2C_RDWR) */
int hw_i2c_enumerate(char* buf, int buflen);             /* 枚举 /dev/i2c-* */

/* ========== PWM (Linux /sys/class/pwm sysfs, 2026-09-07) ========== */
int hw_pwm_export(int chip, int channel);                /* pwmchip{chip}/export -> handle(>=0) */
int hw_pwm_unexport(int chip, int channel);
int hw_pwm_set_period(int chip, int channel, int period_ns);
int hw_pwm_set_duty(int chip, int channel, int duty_ns);
int hw_pwm_set_enable(int chip, int channel, int on);
int hw_pwm_read(int chip, int channel, const char* attr,
                char* buf, int buflen);                  /* 读 period/duty_cycle/enable */
int hw_pwm_enumerate(char* buf, int buflen);             /* 枚举 /sys/class/pwm/pwmchip* */

/* ========== GPIO (sysfs) ========== */
int hw_gpio_export(int pin);
int hw_gpio_set_direction(int pin, int output);
int hw_gpio_write(int pin, int val);
int hw_gpio_read(int pin);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_HW_DIRECT_H */