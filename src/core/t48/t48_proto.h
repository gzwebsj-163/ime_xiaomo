/*
 * t48_proto.h - XGecu T48 烧录器·自研驱动 命令协议层
 *
 * 基于 T48 协议事实实现（clean-room）：
 *   命令格式：64 字节消息头（EP01 OUT / EP81 IN）
 *   载荷传输：EP02/EP03 分片+去交错
 *
 * 用法：
 *   t48_handle_t *h = t48_open("W25Q128", 1);
 *   t48_read(h, buf, 0, 0x1000);  // 读 4KB
 *   t48_write(h, buf, 0, 0x1000); // 写 4KB
 *   t48_erase_chip(h);             // 全片擦除
 *   t48_close(h);
 */

#ifndef T48_PROTO_H
#define T48_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* 内存区类型（T48_MEM_CODE/DATA/USER）定义在 t48_db.h 中 */
#include "t48_db.h"

/* 操作结果码 */
#define T48_OK           0
#define T48_ERR_OPEN     -1  /* 打开/枚举失败 */
#define T48_ERR_PROTO    -2  /* 协议错误 */
#define T48_ERR_OVC      -3  /* 过流保护 */
#define T48_ERR_CHIPID   -4  /* 芯片 ID 不匹配 */
#define T48_ERR_MEM      -5  /* 内存不足 */
#define T48_ERR_PARAM    -6  /* 参数错误 */
#define T48_ERR_IO       -7  /* USB I/O 错误 */

/* 编程状态回调（NULL=不回调） */
typedef void (*t48_progress_fn)(int percent, const char *msg);

/* 不透明句柄 */
typedef struct t48_handle t48_handle_t;

/* ========== 生命周期 ========== */

/*
 * 打开 T48 并准备烧录指定芯片。
 *   chip_name: 芯片名（如 "W25Q128"），NULL=只探测不选芯片
 *   verbose: 0=静默 1=打印信息
 *   返回句柄，失败返回 NULL
 */
t48_handle_t *t48_open(const char *chip_name, int verbose);

/* 关闭 T48，释放资源 */
void t48_close(t48_handle_t *h);

/* 探测 T48 是否在线，返回在线数量 */
int t48_probe(int verbose);

/* ========== 芯片操作 ========== */

/*
 * 开始事务（设置 ZIF 引脚电压/协议等，必须在读写擦前调用）
 *   返回 0 成功
 */
int t48_begin_transaction(t48_handle_t *h);

/* 结束事务（释放 ZIF 引脚） */
int t48_end_transaction(t48_handle_t *h);

/*
 * 读取芯片 ID（验证芯片是否正确插入）
 *   out_id: 输出芯片 ID（如 SPI Flash 的 JEDEC ID）
 *   返回 0 成功
 */
int t48_read_chip_id(t48_handle_t *h, uint32_t *out_id);

/*
 * SPI 自动检测（自动识别插入的 SPI Flash 型号）
 *   out_id: 输出 JEDEC ID
 *   返回 0 成功
 */
int t48_spi_autodetect(t48_handle_t *h, uint32_t *out_id);

/*
 * 读取数据块
 *   mem_type: T48_MEM_CODE / DATA / USER
 *   addr: 起始地址
 *   data: 输出缓冲区
 *   size: 读取大小
 *   返回 0 成功
 */
int t48_read(t48_handle_t *h, int mem_type,
             uint32_t addr, uint8_t *data, size_t size);

/*
 * 写入数据块
 *   mem_type: T48_MEM_CODE / DATA / USER
 *   addr: 起始地址
 *   data: 输入数据
 *   size: 写入大小
 *   返回 0 成功
 */
int t48_write(t48_handle_t *h, int mem_type,
              uint32_t addr, const uint8_t *data, size_t size);

/* 全片擦除 */
int t48_erase_chip(t48_handle_t *h);

/* 保护关（允许写入） */
int t48_protect_off(t48_handle_t *h);

/* 保护开（禁止写入） */
int t48_protect_on(t48_handle_t *h);

/* 获取过流/状态 */
int t48_get_status(t48_handle_t *h, uint8_t *ovc_out);

/* ========== 高级操作 ========== */

/*
 * 整片读取（含进度回调）
 *   data: 输出缓冲区（必须 >= chip_size）
 *   返回实际读取的字节数，<0 表示错误
 */
int t48_read_all(t48_handle_t *h, uint8_t *data, t48_progress_fn progress);

/*
 * 整片写入（含进度回调，自动先擦除）
 *   data: 输入数据
 *   size: 数据大小
 *   返回 0 成功
 */
int t48_write_all(t48_handle_t *h, const uint8_t *data, size_t size,
                  t48_progress_fn progress);

/*
 * 整片校验（与已烧录内容比较）
 *   data: 期望数据
 *   size: 数据大小
 *   返回 0=一致，>0=第一个不一致的偏移，<0=错误
 */
int t48_verify(t48_handle_t *h, const uint8_t *data, size_t size,
               t48_progress_fn progress);

/* 获取当前芯片名 */
const char *t48_chip_name(t48_handle_t *h);

/* 获取芯片容量（字节） */
uint32_t t48_chip_size(t48_handle_t *h);

/* 获取错误信息 */
const char *t48_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* T48_PROTO_H */