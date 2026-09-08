/* byteswap.h — macOS shim for the Linux-only <byteswap.h>
 * TinyEMU needs bswap_16/32/64 on non-Windows hosts, but macOS has no
 * byteswap.h. Map to GCC/Clang builtins (same semantics, undefined for
 * non-power-of-2 sizes which is fine here).
 */
#ifndef BYTESWAP_H
#define BYTESWAP_H

#include <stdint.h>

static inline uint16_t bswap_16(uint16_t v)
{
    return __builtin_bswap16(v);
}

static inline uint32_t bswap_32(uint32_t v)
{
    return __builtin_bswap32(v);
}

static inline uint64_t bswap_64(uint64_t v)
{
    return __builtin_bswap64(v);
}

#endif /* BYTESWAP_H */
