/*
 * xiaomo - OEM 十六进制工具 (hw_hex)
 *
 * 设备签名/寄存器值的 hex 格式化辅助。
 * 与 hw_oem (OEM 熔丝层) 配套使用。
 */
#ifndef XIAOMO_HW_HEX_H
#define XIAOMO_HW_HEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 64 位值 → "0x........" 小写 hex 字符串 (out 至少 19 字节), 返回 out */
char* hw_hex_fmt64(uint64_t v, char* out, int outlen);

/* "0x..." hex 字符串 → 64 位值 (非法字符停止), 返回解析值 */
uint64_t hw_hex_parse64(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_HW_HEX_H */
