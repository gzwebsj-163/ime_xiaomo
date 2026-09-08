/*
 * xiaomo - OEM 十六进制工具 (hw_hex)
 *
 * 设备签名/寄存器值的 hex 格式化辅助 (熔丝层配套)。
 */
#include "hw_hex.h"
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char* hw_hex_fmt64(uint64_t v, char* out, int outlen)
{
    if (!out || outlen <= 0) return out;
    snprintf(out, (size_t)outlen, "0x%016llx", (unsigned long long)v);
    return out;
}

uint64_t hw_hex_parse64(const char* s)
{
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t v = 0;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

#ifdef __cplusplus
}
#endif
