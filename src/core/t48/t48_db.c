/*
 * t48_db.c - XGecu T48·自研驱动 设备数据库实现
 *
 * 内置芯片参数提取自 minipro infoic.xml（T48 数据集），9 款常用：
 *   - SPI NOR Flash: W25Q32/64/128, GD25Q32/64, SST25VF016B
 *   - I2C EEPROM:    24C02/04/08/16
 * 后续可扩展（解析完整 infoic.xml 生成）。
 */

#include "t48_db.h"

#include <stdio.h>
#include <string.h>

/* SPI Flash 共性参数（T48） */
#define SPI_FLASH_PROTO 0x03
#define SPI_FLASH_VARIANT 0x1102
#define SPI_FLASH_RBS 0x1000
#define SPI_FLASH_WBS 0x0100
#define SPI_FLASH_PAGE 0x0100
#define SPI_FLASH_VOLT 0x0001
#define SPI_FLASH_PULSE 0x1388
#define SPI_FLASH_FLAGS 0x00504278
#define SPI_FLASH_INFO 0x0090
#define SPI_FLASH_PINMAP 0x00000301
#define SPI_FLASH_PKG 0x08000900
#define SPI_CLK_8MHZ 0x01

/* I2C EEPROM 共性参数（T48） */
#define I2C_PROTO 0x01
#define I2C_VARIANT 0x1300
#define I2C_RBS 0x0080
#define I2C_VOLT 0x0000
#define I2C_PULSE 0x2710
#define I2C_FLAGS 0x00100000
#define I2C_INFO 0x0000
#define I2C_PINMAP 0x0000806b
#define I2C_PKG 0x08000b00

static const t48_device_t t48_db[] = {
    /* ---------------- SPI NOR Flash ---------------- */
    { "W25Q32",  T48_CHIP_EEPROM, SPI_FLASH_PROTO, SPI_FLASH_VARIANT,
      SPI_FLASH_RBS, SPI_FLASH_WBS, 0x400000, 0, SPI_FLASH_PAGE, 0x0002,
      0x00ef4016, 3, SPI_FLASH_VOLT, SPI_FLASH_PULSE, SPI_FLASH_FLAGS,
      SPI_FLASH_INFO, SPI_FLASH_PINMAP, SPI_FLASH_PKG, SPI_CLK_8MHZ, 1 },
    { "W25Q64",  T48_CHIP_EEPROM, SPI_FLASH_PROTO, SPI_FLASH_VARIANT,
      SPI_FLASH_RBS, SPI_FLASH_WBS, 0x800000, 0, SPI_FLASH_PAGE, 0x0002,
      0x00ef4017, 3, SPI_FLASH_VOLT, SPI_FLASH_PULSE, SPI_FLASH_FLAGS,
      SPI_FLASH_INFO, SPI_FLASH_PINMAP, SPI_FLASH_PKG, SPI_CLK_8MHZ, 1 },
    { "W25Q128", T48_CHIP_EEPROM, SPI_FLASH_PROTO, SPI_FLASH_VARIANT,
      SPI_FLASH_RBS, SPI_FLASH_WBS, 0x1000000, 0, SPI_FLASH_PAGE, 0x0002,
      0x00ef4018, 3, SPI_FLASH_VOLT, SPI_FLASH_PULSE, SPI_FLASH_FLAGS,
      SPI_FLASH_INFO, SPI_FLASH_PINMAP, SPI_FLASH_PKG, SPI_CLK_8MHZ, 1 },
    { "GD25Q32", T48_CHIP_EEPROM, SPI_FLASH_PROTO, SPI_FLASH_VARIANT,
      SPI_FLASH_RBS, SPI_FLASH_WBS, 0x400000, 0, SPI_FLASH_PAGE, 0x0003,
      0x00c84016, 3, SPI_FLASH_VOLT, SPI_FLASH_PULSE, SPI_FLASH_FLAGS,
      SPI_FLASH_INFO, SPI_FLASH_PINMAP, SPI_FLASH_PKG, SPI_CLK_8MHZ, 1 },
    { "GD25Q64", T48_CHIP_EEPROM, SPI_FLASH_PROTO, SPI_FLASH_VARIANT,
      SPI_FLASH_RBS, SPI_FLASH_WBS, 0x800000, 0, SPI_FLASH_PAGE, 0x0003,
      0x00c84017, 3, SPI_FLASH_VOLT, SPI_FLASH_PULSE, SPI_FLASH_FLAGS,
      SPI_FLASH_INFO, SPI_FLASH_PINMAP, SPI_FLASH_PKG, SPI_CLK_8MHZ, 1 },
    { "SST25VF016B", T48_CHIP_EEPROM, 0x0f, 0x8132,
      0x1000, 0x0100, 0x200000, 0, 0x0100, 0x0000,
      0x00bf2541, 3, 0x0001, 0x000a, 0x00004030,
      0x0000, 0x00000301, 0x08000000, SPI_CLK_8MHZ, 1 },
    /* ---------------- I2C EEPROM ---------------- */
    { "24C02",  T48_CHIP_EEPROM, I2C_PROTO, I2C_VARIANT,
      I2C_RBS, 0x0008, 0x0100, 0, 0x0008, 0x0000,
      0x00000000, 0, I2C_VOLT, I2C_PULSE, I2C_FLAGS,
      I2C_INFO, I2C_PINMAP, I2C_PKG, 0, 0 },
    { "24C04",  T48_CHIP_EEPROM, I2C_PROTO, I2C_VARIANT,
      I2C_RBS, 0x0010, 0x0200, 0, 0x0010, 0x0000,
      0x00000000, 0, I2C_VOLT, I2C_PULSE, I2C_FLAGS,
      I2C_INFO, I2C_PINMAP, I2C_PKG, 0, 0 },
    { "24C08",  T48_CHIP_EEPROM, I2C_PROTO, I2C_VARIANT,
      I2C_RBS, 0x0010, 0x0400, 0, 0x0010, 0x0000,
      0x00000000, 0, I2C_VOLT, I2C_PULSE, I2C_FLAGS,
      I2C_INFO, I2C_PINMAP, I2C_PKG, 0, 0 },
    { "24C16",  T48_CHIP_EEPROM, I2C_PROTO, I2C_VARIANT,
      I2C_RBS, 0x0010, 0x0800, 0, 0x0010, 0x0000,
      0x00000000, 0, I2C_VOLT, I2C_PULSE, I2C_FLAGS,
      I2C_INFO, I2C_PINMAP, I2C_PKG, 0, 0 },
};

#define T48_DB_N (sizeof(t48_db) / sizeof(t48_db[0]))

static int name_match(const char *a, const char *b)
{
    /* 大小写不敏感，且支持前缀匹配（"w25q32" 命中 "W25Q32"） */
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return 1;
}

const t48_device_t *t48_db_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < T48_DB_N; i++) {
        if (name_match(t48_db[i].name, name))
            return &t48_db[i];
    }
    return NULL;
}

const t48_device_t *t48_db_find_by_id(uint32_t id)
{
    uint32_t want = id & 0x00ffffff;
    for (size_t i = 0; i < T48_DB_N; i++) {
        if (t48_db[i].chip_id && (t48_db[i].chip_id & 0x00ffffff) == want)
            return &t48_db[i];
    }
    return NULL;
}

void t48_db_list(void)
{
    printf("%-16s %-5s %8s %8s %10s  %s\n",
           "芯片", "类型", "主区大小", "页大小", "芯片ID", "协议");
    for (size_t i = 0; i < T48_DB_N; i++) {
        const t48_device_t *d = &t48_db[i];
        printf("%-16s %-5s %8u %8u %10s  %s\n",
               d->name,
               d->type == T48_CHIP_EEPROM ? "EEPROM" : "MCU",
               d->code_memory_size, d->page_size,
               d->chip_id ? "有" : "无",
               d->protocol_id == 0x03 ? "SPI" :
               d->protocol_id == 0x01 ? "I2C" : "其它");
    }
}
