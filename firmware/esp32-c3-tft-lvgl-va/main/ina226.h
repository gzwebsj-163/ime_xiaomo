/**
 * INA226 电压/电流/功率检测驱动（I2C）
 * - 10mΩ 分流电阻，CAL=51200 → 电流 LSB = 0.25mA
 * - 总线电压 LSB = 1.25mV，分流电压 LSB = 2.5uV
 * - I2C 地址 0x40
 */
#pragma once

#include <stdbool.h>

typedef struct {
    float volt;      /* 总线电压 V   */
    float curr;      /* 电流 A       */
    float power;     /* 功率 W       */
} ina226_data_t;

/** 初始化 I2C 总线 + INA226 配置寄存器 */
bool ina226_init(int sda_gpio, int scl_gpio, int i2c_port);

/** 读取一次电压/电流/功率，返回 false 表示设备不在线 */
bool ina226_read(ina226_data_t *d);
