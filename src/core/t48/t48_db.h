/*
 * t48_db.h - XGecu T48·自研驱动 设备数据库
 *
 * 内置常用芯片参数（提取自 minipro infoic.xml 的 T48 数据集）。
 */

#ifndef T48_DB_H
#define T48_DB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 芯片类型 */
enum {
    T48_CHIP_EEPROM = 1,   /* 含 24C I2C / 25xx SPI */
    T48_CHIP_MCU = 2,
    T48_CHIP_NAND = 6,
};

/* 内存区类型（read/write block 用） */
enum {
    T48_MEM_CODE = 0,   /* 主程序区 / Flash */
    T48_MEM_DATA = 1,
    T48_MEM_USER = 2,
};

/* 设备参数 —— 字段布局严格对齐 BEGIN_TRANS 报文，勿随意增删改位 */
typedef struct {
    const char *name;        /* 芯片名（用户输入用，大小写不敏感） */
    uint8_t  type;           /* 芯片类型 */
    uint8_t  protocol_id;    /* 协议 ID（0x01=24C I2C, 0x03=25xx SPI） */
    uint16_t variant;        /* 变体 */
    uint16_t read_buffer_size;  /* 读缓冲（一次读块最大） */
    uint16_t write_buffer_size; /* 写缓冲（一次写块最大） */
    uint32_t code_memory_size;  /* 主区大小 */
    uint32_t data_memory_size;
    uint16_t page_size;
    uint16_t pages_per_block;
    uint32_t chip_id;        /* 期望芯片 ID（0=不校验） */
    uint8_t  chip_id_bytes_count;
    uint32_t voltages;       /* 电压原始编码 */
    uint16_t pulse_delay;
    uint32_t flags;
    uint8_t  chip_info;
    uint32_t pin_map;
    uint32_t package_details;
    uint8_t  spi_clock;      /* SPI 时钟编码（T48: 0x01=8MHz） */
    uint8_t  can_adjust_clock;
} t48_device_t;

/* 按名查找（支持小写/前缀）。 */
const t48_device_t *t48_db_find(const char *name);
/* 按芯片 ID 查找（SPI JEDEC ID 高 24 位匹配）。 */
const t48_device_t *t48_db_find_by_id(uint32_t id);
void t48_db_list(void);

#ifdef __cplusplus
}
#endif

#endif /* T48_DB_H */
