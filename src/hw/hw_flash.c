/*
 * xiaomo - ESP32 ROM 下载协议烧录层 (hw_flash, 2026-09-30)
 *
 * 把「给 ESP32 烧固件」从外部工具 (esptool/Python) 收进 xiaomo 架构本身:
 *   纯 C 全跨式六模式 (HOST/LINUX/KELL/ESP32/ESP8266/TEST),
 *   烧录可由 .mo 字节码驱动  hw_flash("run 4096")。
 *
 * 协议面与 esptool loader.py 同源, 四条铁律 (详见
 * knowledge/concepts/esp32-sim-flasher-rom-protocol.md):
 *   1. SLIP RFC1055: 字面 C0 -> DB DC, 字面 DB -> DB DD (方向不可反)
 *   2. 请求帧 "<BBHI": dir+cmd+len+xor_checksum, body 从 pd+8 起
 *   3. 应答末两字节 [status,error] 不可省
 *   4. ROM 无 stub: 块=1024B, 不收 FLASH_END, 最后一条是 SPI_FLASH_MD5(0x13)
 *
 * 全跨式设计:
 *   - 核心只依赖 stdint+string (+ stdio/stdlib 仅 CLI/自检用)
 *   - 底层字节收发走 BSP 回调; 默认 = 确定性 ROM 模拟器 (S3 ROM 语义,
 *     表驱动, 全平台逐位一致可回归); 真机固件 hw_flash_bsp_install()
 *     注入真实 UART, 上层逻辑零改动
 *   - 黄金参考 HW_FLASH_GOLDEN = 命令表 FNV-1a-32 (Python 对拍锁定)
 *
 * 六层接入: Makefile / OP_HW_FLASH_CALL / mo2kbc 内置 / CLI flash /
 *           examples/flash_test.mo / tests/run_tests.sh
 */
#include "hw_flash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * 1. 编译期模式探测 (全跨式, 与 hw_token/hw_fault 同款)
 * ============================================================ */
static uint8_t fl_mode_probe(void)
{
#if defined(HW_FLASH_MODE_OVERRIDE)
    return (uint8_t)HW_FLASH_MODE_OVERRIDE;
#elif defined(HW_FLASH_KELL)
    return (uint8_t)HW_FLASH_MODE_KELL;
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
    return (uint8_t)HW_FLASH_MODE_ESP8266;
#elif defined(CONFIG_IDF_TARGET_ESP32)  || defined(CONFIG_IDF_TARGET_ESP32S2) || \
      defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2)
    return (uint8_t)HW_FLASH_MODE_ESP32;
#elif defined(__linux__)
    return (uint8_t)HW_FLASH_MODE_LINUX;
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
    return (uint8_t)HW_FLASH_MODE_HOST;
#else
    return (uint8_t)HW_FLASH_MODE_TEST;
#endif
}

uint8_t hw_flash_mode(void)
{
    static uint8_t cached = 0xFFu;
    if (cached == 0xFFu) cached = fl_mode_probe();
    return cached;
}

static const char* const fl_mode_names[HW_FLASH_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};

const char* hw_flash_mode_str(uint8_t mode)
{
    return (mode < HW_FLASH_MODE_MAX) ? fl_mode_names[mode] : "?";
}

const char* hw_flash_result_code_str(int rc)
{
    switch (rc) {
    case HW_FLASH_R_OK:       return "OK";
    case HW_FLASH_R_NOARGS:   return "NOARGS";
    case HW_FLASH_R_BADARG:   return "BADARG";
    case HW_FLASH_R_IOERR:    return "IOERR";
    case HW_FLASH_R_PROTO:    return "PROTO";
    case HW_FLASH_R_CHECKSUM: return "CHECKSUM";
    case HW_FLASH_R_BADSIZE:  return "BADSIZE";
    case HW_FLASH_R_NOCMD:    return "NOCMD";
    case HW_FLASH_R_HELP:     return "HELP";
    default:                  return "?";
    }
}

/* ============================================================
 * 2. ROM 命令表 + 黄金校验和 (FNV-1a-32)
 * ============================================================ */
static const hw_flash_cmd_info_t g_cmd_table[] = {
    { HW_FLASH_CMD_FLASH_BEGIN, "FLASH_BEGIN" },
    { HW_FLASH_CMD_FLASH_DATA,  "FLASH_DATA"  },
    { HW_FLASH_CMD_FLASH_END,   "FLASH_END"   },
    { HW_FLASH_CMD_SYNC,        "SYNC"        },
    { HW_FLASH_CMD_WRITE_REG,   "WRITE_REG"   },
    { HW_FLASH_CMD_READ_REG,    "READ_REG"    },
    { HW_FLASH_CMD_CHANGE_BAUD, "CHANGE_BAUD" },
    { HW_FLASH_CMD_FLASH_MD5,   "FLASH_MD5"   },
    { HW_FLASH_CMD_SEC_INFO,    "SEC_INFO"    }
};

const hw_flash_cmd_info_t* hw_flash_cmd_table(void) { return g_cmd_table; }
uint32_t hw_flash_cmd_count(void)
{
    return (uint32_t)(sizeof(g_cmd_table) / sizeof(g_cmd_table[0]));
}

uint32_t hw_flash_cmd_checksum(void)
{
    uint32_t h = 2166136261u;
    uint32_t i, k;
    for (i = 0; i < hw_flash_cmd_count(); i++) {
        const char* s = g_cmd_table[i].name;
        h ^= (uint32_t)g_cmd_table[i].code;
        h *= 16777619u;
        for (k = 0; s[k] != '\0'; k++) {
            h ^= (uint32_t)(uint8_t)s[k];
            h *= 16777619u;
        }
    }
    return h;
}

static const char* fl_cmd_name(uint8_t code)
{
    uint32_t i;
    for (i = 0; i < hw_flash_cmd_count(); i++)
        if (g_cmd_table[i].code == code) return g_cmd_table[i].name;
    return "?";
}

/* ============================================================
 * 3. XOR 校验和 (ESP_CHECKSUM_MAGIC 0xEF) + 测试镜像
 * ============================================================ */
uint8_t hw_flash_checksum(const uint8_t* d, uint32_t n)
{
    uint8_t c = HW_FLASH_CHECKSUM_MAGIC;
    uint32_t i;
    if (!d) return c;
    for (i = 0; i < n; i++) c ^= d[i];
    return c;
}

/* 确定性测试镜像: b[i] = (i*73+11)&0xFF, 每 97 字节置 C0, 每 131 置 DB
 * (刻意含 SLIP 转义字节, 用于回归 RFC1055 表方向) */
uint32_t hw_flash_test_image(uint8_t* buf, uint32_t n)
{
    uint32_t i;
    if (!buf) return 0;
    for (i = 0; i < n; i++) {
        uint8_t v = (uint8_t)((i * 73u + 11u) & 0xFFu);
        if (i % 97u == 0u)       v = (uint8_t)HW_FLASH_SLIP_END;
        else if (i % 131u == 0u) v = (uint8_t)HW_FLASH_SLIP_ESC;
        buf[i] = v;
    }
    return n;
}

/* ============================================================
 * 4. MD5 (RFC1321, 纯整数实现, 无动态内存)
 * ============================================================ */
typedef struct {
    uint32_t h[4];
    uint64_t len;          /* 已处理位长 */
    uint8_t  buf[64];
    uint32_t buflen;
} fl_md5_ctx;

static const uint32_t fl_md5_k[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};

static const uint8_t fl_md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static uint32_t fl_rotl32(uint32_t x, uint32_t c)
{
    return (x << c) | (x >> (32u - c));
}

static void fl_md5_block(fl_md5_ctx* c, const uint8_t* p)
{
    uint32_t m[16], a, b, cc, d, f, tmp;
    uint32_t i, g;
    for (i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    for (i = 0; i < 64; i++) {
        if (i < 16)      { f = (b & cc) | (~b & d);        g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);        g = (5u*i + 1u) & 15u; }
        else if (i < 48) { f = b ^ cc ^ d;                 g = (3u*i + 5u) & 15u; }
        else             { f = cc ^ (b | ~d);              g = (7u*i) & 15u; }
        tmp = d; d = cc; cc = b;
        b = b + fl_rotl32(a + f + fl_md5_k[i] + m[g], fl_md5_s[i]);
        a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
}

static void fl_md5_init(fl_md5_ctx* c)
{
    c->h[0] = 0x67452301u; c->h[1] = 0xefcdab89u;
    c->h[2] = 0x98badcfeu; c->h[3] = 0x10325476u;
    c->len = 0; c->buflen = 0;
}

static void fl_md5_update(fl_md5_ctx* c, const uint8_t* d, uint32_t n)
{
    uint32_t i = 0;
    if (!d) return;
    while (i < n) {
        uint32_t take = 64u - c->buflen;
        if (take > (n - i)) take = n - i;
        memcpy(c->buf + c->buflen, d + i, take);
        c->buflen += take; i += take;
        if (c->buflen == 64u) { fl_md5_block(c, c->buf); c->buflen = 0; }
    }
    c->len += (uint64_t)n * 8u;
}

static void fl_md5_final(fl_md5_ctx* c, uint8_t out16[16])
{
    uint8_t pad[72];
    uint32_t padlen, i;
    uint64_t bits = c->len;
    pad[0] = 0x80u;
    padlen = ((c->buflen < 56u) ? (56u - c->buflen) : (120u - c->buflen));
    for (i = 1; i < padlen; i++) pad[i] = 0x00u;
    for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)((bits >> (8u*i)) & 0xFFu);
    fl_md5_update(c, pad, padlen + 8u);
    for (i = 0; i < 4; i++) {
        out16[i*4]     = (uint8_t)( c->h[i]        & 0xFFu);
        out16[i*4 + 1] = (uint8_t)((c->h[i] >> 8)  & 0xFFu);
        out16[i*4 + 2] = (uint8_t)((c->h[i] >> 16) & 0xFFu);
        out16[i*4 + 3] = (uint8_t)((c->h[i] >> 24) & 0xFFu);
    }
}

void hw_flash_md5(const uint8_t* d, uint32_t n, uint8_t out16[16])
{
    fl_md5_ctx c;
    fl_md5_init(&c);
    fl_md5_update(&c, d, n);
    fl_md5_final(&c, out16);
}

void hw_flash_md5_hex(const uint8_t* d, uint32_t n, char out33[33])
{
    static const char* hx = "0123456789abcdef";
    uint8_t dig[16];
    uint32_t i;
    if (!out33) return;
    hw_flash_md5(d, n, dig);
    for (i = 0; i < 16; i++) {
        out33[i*2]     = hx[(dig[i] >> 4) & 0x0Fu];
        out33[i*2 + 1] = hx[dig[i] & 0x0Fu];
    }
    out33[32] = '\0';
}

/* ============================================================
 * 5. SLIP (RFC1055) 编解码
 * ============================================================ */
int hw_flash_slip_encode(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap)
{
    uint32_t o = 0, i;
    if (!out || (n > 0u && !in)) return -1;
    if (cap < 2u) return -1;
    out[o++] = (uint8_t)HW_FLASH_SLIP_END;
    for (i = 0; i < n; i++) {
        uint8_t b = in[i];
        if (b == (uint8_t)HW_FLASH_SLIP_END) {
            if (o + 2u > cap) return -1;
            out[o++] = (uint8_t)HW_FLASH_SLIP_ESC;
            out[o++] = (uint8_t)HW_FLASH_SLIP_ESC_END;   /* C0 -> DB DC */
        } else if (b == (uint8_t)HW_FLASH_SLIP_ESC) {
            if (o + 2u > cap) return -1;
            out[o++] = (uint8_t)HW_FLASH_SLIP_ESC;
            out[o++] = (uint8_t)HW_FLASH_SLIP_ESC_ESC;   /* DB -> DB DD */
        } else {
            if (o + 1u > cap) return -1;
            out[o++] = b;
        }
    }
    if (o + 1u > cap) return -1;
    out[o++] = (uint8_t)HW_FLASH_SLIP_END;
    return (int)o;
}

int hw_flash_slip_decode(const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap)
{
    uint32_t i = 0, o = 0;
    int esc = 0;
    if (!in || !out) return -1;
    /* 跳过前导定界符 */
    while (i < n && in[i] == (uint8_t)HW_FLASH_SLIP_END) i++;
    for (; i < n; i++) {
        uint8_t b = in[i];
        if (b == (uint8_t)HW_FLASH_SLIP_END) break;   /* 收尾定界 */
        if (esc) {
            if (b == (uint8_t)HW_FLASH_SLIP_ESC_END)      b = (uint8_t)HW_FLASH_SLIP_END;
            else if (b == (uint8_t)HW_FLASH_SLIP_ESC_ESC) b = (uint8_t)HW_FLASH_SLIP_ESC;
            else return -1;                            /* 非法转义 */
            esc = 0;
        } else if (b == (uint8_t)HW_FLASH_SLIP_ESC) {
            esc = 1;
            continue;
        }
        if (o + 1u > cap) return -1;
        out[o++] = b;
    }
    if (esc) return -1;
    return (int)o;
}

/* ============================================================
 * 6. 请求帧 "<BBHI" 与应答帧解析
 * ============================================================ */
int hw_flash_frame_build(uint8_t cmd, const uint8_t* body, uint32_t blen,
                         uint8_t* out, uint32_t cap)
{
    uint8_t ck;
    uint32_t i;
    if (!out) return -1;
    if (blen > 0u && !body) return -1;
    if (blen > 0xFFFFu) return -1;
    if (cap < HW_FLASH_HDR_LEN + blen) return -1;
    ck = hw_flash_checksum(body, blen);
    out[0] = (uint8_t)HW_FLASH_DIR_REQ;
    out[1] = cmd;
    out[2] = (uint8_t)(blen & 0xFFu);
    out[3] = (uint8_t)((blen >> 8) & 0xFFu);
    out[4] = ck;
    out[5] = 0x00u;
    out[6] = 0x00u;
    out[7] = 0x00u;
    for (i = 0; i < blen; i++) out[HW_FLASH_HDR_LEN + i] = body[i];
    return (int)(HW_FLASH_HDR_LEN + blen);
}

int hw_flash_frame_parse(const uint8_t* in, uint32_t n, uint8_t* cmd, uint32_t* val,
                         const uint8_t** body, uint32_t* blen, uint8_t* st)
{
    uint32_t len;
    if (!in || n < HW_FLASH_HDR_LEN + HW_FLASH_RSP_TAIL) return -1;
    if (in[0] != (uint8_t)HW_FLASH_DIR_RSP) return -1;
    if (cmd) *cmd = in[1];
    len = (uint32_t)in[2] | ((uint32_t)in[3] << 8);          /* data 长度 */
    if (val) *val = (uint32_t)in[4] | ((uint32_t)in[5] << 8) |
                    ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
    if (HW_FLASH_HDR_LEN + len + HW_FLASH_RSP_TAIL > n) return -1;
    if (body) *body = in + HW_FLASH_HDR_LEN;
    if (blen) *blen = len;
    if (st)   *st   = in[HW_FLASH_HDR_LEN + len];            /* status 字节 */
    return (int)(HW_FLASH_HDR_LEN + len + HW_FLASH_RSP_TAIL);
}

/* ============================================================
 * 7. ROM 模拟器 (设备侧确定性实现) —— 与已验证的 IDE 模拟器
 *    (esp32-sim/src/pty_sim.c rom_reply/handle_cmd) 语义逐条对齐,
 *    但零文件 IO / 零 SSL: flash 模型驻留静态数组, 全平台逐位一致。
 * ============================================================ */
static hw_flash_bsp_t g_bsp;              /* 函数指针全空 = 用模拟器 */
static uint8_t  g_rsp[HW_FLASH_SLIP_CAP]; /* 模拟器 -> 上层 的 SLIP 应答 */
static uint32_t g_rsp_len, g_rsp_pos;
static uint8_t  g_model[HW_FLASH_MODEL_CAP];
static uint32_t g_model_len;              /* 本次会话镜像长度 */
static uint32_t g_model_off;              /* flash 起始偏移 */
static uint32_t g_model_blk;              /* FLASH_BEGIN 声明的块尺寸 */
static uint8_t  g_spi_cmd;                /* WRITE_REG 记下的 SPI_USR2 子命令 */

/* 编码一条 ROM 应答: 01 op len(u16) val(u32) | data[len] | st err → SLIP */
static uint32_t sim_reply(uint8_t op, uint32_t val, const uint8_t* data, uint32_t len)
{
    uint8_t raw[HW_FLASH_HDR_LEN + 64u + HW_FLASH_RSP_TAIL];
    uint32_t o = 0, i;
    int e;
    if (len > 64u) return 0;
    raw[o++] = (uint8_t)HW_FLASH_DIR_RSP;
    raw[o++] = op;
    raw[o++] = (uint8_t)(len & 0xFFu);
    raw[o++] = (uint8_t)((len >> 8) & 0xFFu);
    raw[o++] = (uint8_t)( val        & 0xFFu);
    raw[o++] = (uint8_t)((val >> 8)  & 0xFFu);
    raw[o++] = (uint8_t)((val >> 16) & 0xFFu);
    raw[o++] = (uint8_t)((val >> 24) & 0xFFu);
    for (i = 0; i < len; i++) raw[o++] = data[i];
    raw[o++] = (uint8_t)HW_FLASH_ST_OK;    /* 铁律: 末两字节状态尾 */
    raw[o++] = (uint8_t)HW_FLASH_ST_OK;
    e = hw_flash_slip_encode(raw, o, g_rsp, (uint32_t)sizeof(g_rsp));
    if (e > 0) { g_rsp_len = (uint32_t)e; g_rsp_pos = 0; return (uint32_t)e; }
    return 0u;
}

/* READ_REG 精确语义 (错一个地址 esptool 就死在探测/轮询) */
static uint32_t sim_read_reg(uint32_t addr)
{
    if (addr == (uint32_t)HW_FLASH_SPI_MAGIC_ADDR) return HW_FLASH_CHIP_ID;   /* 0x60001F10 -> 9 */
    if (addr == (uint32_t)HW_FLASH_SPI_CMD_ADDR)   return 0u;                /* SPI_CMD_REG 空闲 */
    if (addr == (uint32_t)HW_FLASH_SPI_RDID_ADDR)                            /* W0: RDID 结果 */
        return (g_spi_cmd == 0x9Fu) ? HW_FLASH_RDID_WINBOND : 0u;
    if (addr >= 0x60007000u && addr < 0x60007700u) return 0x5e0508b7u;       /* efuse MAC/rev */
    return 0u;
}

static uint32_t rd_u32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 模拟器命令处理: 收到一条已解 SLIP 的请求帧 */
static void sim_handle(const uint8_t* pd, uint32_t n)
{
    uint8_t cmd;
    const uint8_t* body;
    uint32_t blen;
    if (n < HW_FLASH_HDR_LEN) return;
    cmd  = pd[1];
    body = pd + HW_FLASH_HDR_LEN;
    blen = n - HW_FLASH_HDR_LEN;

    switch (cmd) {
    case HW_FLASH_CMD_SYNC:                      /* 真 ROM 回 8 帧, val=0x20121220 */
    {
        uint32_t i, tot = 0;
        for (i = 0; i < 8u; i++) {
            uint32_t e = sim_reply(HW_FLASH_CMD_SYNC, 0x20121220u, NULL, 0);
            if (e) tot = e;                      /* 只保留最后一帧给上层读 */
        }
        (void)tot;
        break;
    }
    case HW_FLASH_CMD_READ_REG:
        if (blen >= 4u) sim_reply(cmd, sim_read_reg(rd_u32le(body)), NULL, 0);
        else            sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_WRITE_REG:
        if (blen >= 8u) {
            uint32_t addr = rd_u32le(body);
            uint32_t wv   = rd_u32le(body + 4);
            if (addr == 0x60002020u) g_spi_cmd = (uint8_t)(wv & 0xFFu);
        }
        sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_FLASH_BEGIN:               /* total blocks bsize offset [enc?] */
        if (blen >= 16u) {
            g_model_len = rd_u32le(body);
            g_model_blk = rd_u32le(body + 8);
            g_model_off = rd_u32le(body + 12);
            if (g_model_len > HW_FLASH_MODEL_CAP) g_model_len = HW_FLASH_MODEL_CAP;
            memset(g_model, 0xFF, sizeof(g_model));   /* 擦除 */
        }
        sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_FLASH_DATA:               /* size seq 0 0 | data[bsize] */
        if (blen >= HW_FLASH_DATA_HDR) {
            uint32_t sz  = rd_u32le(body);
            uint32_t seq = rd_u32le(body + 4);
            uint32_t off = seq * g_model_blk;
            uint32_t i;
            if (blen - HW_FLASH_DATA_HDR < sz) sz = blen - HW_FLASH_DATA_HDR;
            for (i = 0; i < sz; i++)
                if (off + i < HW_FLASH_MODEL_CAP) g_model[off + i] = body[HW_FLASH_DATA_HDR + i];
        }
        sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_FLASH_END:
        sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_CHANGE_BAUD:
        sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_FLASH_MD5:                /* addr size -> 32 字节 ASCII hex */
        if (blen >= 8u) {
            uint32_t addr = rd_u32le(body);
            uint32_t size = rd_u32le(body + 4);
            char hex[32];
            const uint8_t* base = g_model + (addr >= g_model_off ? (addr - g_model_off) : 0u);
            {
                static const char* hx = "0123456789abcdef";
                uint32_t i;
                uint8_t d2[16];
                hw_flash_md5(base, size, d2);
                for (i = 0; i < 16u; i++) {
                    hex[i*2]     = hx[(d2[i] >> 4) & 0x0Fu];
                    hex[i*2 + 1] = hx[d2[i] & 0x0Fu];
                }
                sim_reply(cmd, 0u, (const uint8_t*)hex, 32u);
            }
        } else sim_reply(cmd, 0u, NULL, 0);
        break;

    case HW_FLASH_CMD_SEC_INFO:                 /* 20 字节 <IBBBBBBBBII> */
    {
        uint8_t si[20];
        memset(si, 0, sizeof(si));
        si[12] = (uint8_t)HW_FLASH_CHIP_ID;     /* chip_id = ESP32-S3 */
        si[16] = 0x03u;                         /* api_version */
        sim_reply(cmd, 0u, si, 20u);
        break;
    }

    default:
        sim_reply(cmd, 0u, NULL, 0);
        break;
    }
}

/* 默认传输: 上层写 SLIP → 模拟器解 → 处理 → 应答进 g_rsp
 * ⚠️ 大缓冲一律 static (真机加固): 本模块是单实例全局状态机 (g_sess/g_stat/g_model),
 *    把 ~1KB 帧缓冲放栈上, 在 ESP32 小栈任务里会直接踩爆 (实测 IDLE1 栈溢出复位)。 */
static int sim_write(const uint8_t* d, uint32_t n)
{
    static uint8_t dec[HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX + HW_FLASH_RSP_TAIL + 8u];
    int dl = hw_flash_slip_decode(d, n, dec, (uint32_t)sizeof(dec));
    if (dl < 0) return -1;
    g_rsp_len = 0; g_rsp_pos = 0;
    sim_handle(dec, (uint32_t)dl);
    return (int)n;
}

static int sim_read(uint8_t* d, uint32_t cap)
{
    uint32_t avail = g_rsp_len - g_rsp_pos;
    if (avail == 0u) return 0;
    if (avail > cap) avail = cap;
    memcpy(d, g_rsp + g_rsp_pos, avail);
    g_rsp_pos += avail;
    return (int)avail;
}

/* 当前生效的收发 (BSP 优先, 否则模拟器) */
static int fl_tx(const uint8_t* d, uint32_t n)
{
    if (g_bsp.uart_write) return g_bsp.uart_write(d, n);
    return sim_write(d, n);
}

static int fl_rx(uint8_t* d, uint32_t cap)
{
    if (g_bsp.uart_read) return g_bsp.uart_read(d, cap);
    return sim_read(d, cap);
}

/* ============================================================
 * 8. 会话状态 + 协议原语
 * ============================================================ */
static hw_flash_session_t g_sess;
static hw_flash_stat_t    g_stat;
static char               g_last[128];

static void fl_set_result(const char* s)
{
    uint32_t i = 0;
    if (!s) { g_last[0] = '\0'; return; }
    while (s[i] != '\0' && i < (uint32_t)sizeof(g_last) - 1u) { g_last[i] = s[i]; i++; }
    g_last[i] = '\0';
}

void hw_flash_init(void* arg)
{
    (void)arg;
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_stat, 0, sizeof(g_stat));
    memset(&g_bsp, 0, sizeof(g_bsp));
    memset(g_model, 0xFF, sizeof(g_model));
    g_model_len = 0; g_model_off = 0; g_model_blk = HW_FLASH_BLOCK_MAX;
    g_spi_cmd = 0; g_rsp_len = 0; g_rsp_pos = 0;
    fl_set_result("init");
}

void hw_flash_hook(void* arg) { (void)arg; }

void hw_flash_bsp_install(const hw_flash_bsp_t* bsp)
{
    if (bsp) g_bsp = *bsp;
    else     memset(&g_bsp, 0, sizeof(g_bsp));
}

void hw_flash_stat(hw_flash_stat_t* out)
{
    if (out) *out = g_stat;
}

void hw_flash_result(char* buf, uint32_t cap)
{
    uint32_t i;
    if (!buf || cap == 0u) return;
    for (i = 0; i < cap - 1u && g_last[i] != '\0'; i++) buf[i] = g_last[i];
    buf[i] = '\0';
}

uint32_t hw_flash_chip_id(void) { return (uint32_t)g_stat.chip_id; }

/* 发一条 ROM 请求并收应答。返回 0 = OK; >0 = 错误码; -1 = 传输失败 */
static int fl_req(uint8_t cmd, const uint8_t* body, uint32_t blen,
                  uint32_t* out_val, const uint8_t** out_body, uint32_t* out_blen)
{
    /* static (真机加固): 合计 ~6.3KB, 放栈上会把 ESP32 小栈任务直接踩爆 */
    static uint8_t frame[HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX];
    static uint8_t slip[HW_FLASH_SLIP_CAP];
    static uint8_t raw[HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX + 4u];
    static uint8_t rsp[HW_FLASH_SLIP_CAP];
    uint32_t raw_len = 0;
    int fl, dl, got;
    uint8_t rcmd = 0, rst = 0xFFu;
    uint32_t rval = 0;
    const uint8_t* rbody = NULL;
    uint32_t rblen = 0;
    uint32_t acc = 0, tries;

    fl = hw_flash_frame_build(cmd, body, blen, frame, (uint32_t)sizeof(frame));
    if (fl < 0) return HW_FLASH_R_BADARG;
    fl = hw_flash_slip_encode(frame, (uint32_t)fl, slip, (uint32_t)sizeof(slip));
    if (fl < 0) return HW_FLASH_R_BADARG;
    g_stat.cmd_tx++;
    if (fl_tx(slip, (uint32_t)fl) < 0) { fl_set_result("ioerr:tx"); return HW_FLASH_R_IOERR; }

    /* 收: 累积到能解出一整帧为止 (真机会分片到达) */
    got = 0;
    for (tries = 0; tries < 4096u; tries++) {
        int r = fl_rx(rsp + acc, (uint32_t)sizeof(rsp) - acc);
        if (r < 0) { fl_set_result("ioerr:rx"); return HW_FLASH_R_IOERR; }
        if (r == 0) continue;                       /* 暂无数据 (真机 BSP 应有超时) */
        acc += (uint32_t)r;
        dl = hw_flash_slip_decode(rsp, acc, raw, (uint32_t)sizeof(raw));
        if (dl > 0 && (uint32_t)dl >= HW_FLASH_HDR_LEN + HW_FLASH_RSP_TAIL) {
            uint32_t need = HW_FLASH_HDR_LEN + (uint32_t)raw[2] + ((uint32_t)raw[3] << 8)
                            + HW_FLASH_RSP_TAIL;
            if ((uint32_t)dl >= need) {
                raw_len = (uint32_t)dl; got = 1; break;   /* 整帧到齐 */
            }
        }
        if (acc >= sizeof(rsp)) { acc = 0; }
    }
    if (!got) { fl_set_result("proto:no-frame"); return HW_FLASH_R_PROTO; }

    if (hw_flash_frame_parse(raw, raw_len, &rcmd, &rval, &rbody, &rblen, &rst) < 0) {
        fl_set_result("proto:bad-frame");
        return HW_FLASH_R_PROTO;
    }
    g_stat.cmd_rx++;
    if (out_body) *out_body = rbody;      /* ⚠️ 必须判空: sync 等调用传 NULL */
    if (out_val)  *out_val  = rval;
    if (out_blen) *out_blen = rblen;
    if (rst != (uint8_t)HW_FLASH_ST_OK) { fl_set_result("proto:status-fail"); return HW_FLASH_R_PROTO; }
    return HW_FLASH_R_OK;
}

int hw_flash_sync(void)
{
    uint8_t body[36];
    uint32_t i;
    int rc;
    for (i = 0; i < sizeof(body); i++) body[i] = 0x07u;   /* esptool sync 载荷 */
    rc = fl_req(HW_FLASH_CMD_SYNC, body, (uint32_t)sizeof(body), NULL, NULL, NULL);
    fl_set_result(rc == HW_FLASH_R_OK ? "sync ok" : "sync fail");
    return rc;
}

int hw_flash_read_reg(uint32_t addr, uint32_t* val)
{
    uint8_t body[4];
    uint32_t v = 0;
    int rc;
    body[0] = (uint8_t)(addr & 0xFFu);
    body[1] = (uint8_t)((addr >> 8) & 0xFFu);
    body[2] = (uint8_t)((addr >> 16) & 0xFFu);
    body[3] = (uint8_t)((addr >> 24) & 0xFFu);
    rc = fl_req(HW_FLASH_CMD_READ_REG, body, 4u, &v, NULL, NULL);
    /* 探测 magic 地址即落存芯片型号 → 令 chip_id() API 自洽
     * (真机 diag 曾裸调 read_reg 后查 chip_id 得 0, 是 API 语义缺口而非真机差异) */
    if (rc == HW_FLASH_R_OK && addr == (uint32_t)HW_FLASH_SPI_MAGIC_ADDR)
        g_stat.chip_id = (uint8_t)v;
    if (val) *val = v;
    return rc;
}

/* ============================================================
 * 9. 烧录流程 (镜像 -> 设备)
 *    注: ROM 无 stub 模式不发 FLASH_END (stub 专属, esptool cmds.py
 *        IS_STUB 守卫) —— 最后一条协议命令就是 SPI_FLASH_MD5(0x13)。
 * ============================================================ */
int hw_flash_run(const uint8_t* image, uint32_t len, uint32_t offset,
                 hw_flash_session_t* out, hw_flash_progress_fn cb, void* ctx)
{
    /* static (真机加固): body 1KB + begin/md5b/hex, 全放栈上会叠加进
     * hw_flash_run→fl_req 的调用链; 单实例状态机, 复用一份即可 */
    static uint8_t body[HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX];
    uint8_t  begin[20];
    uint8_t  md5b[8];
    char     lhex[33];
    char     rhex[33];
    uint32_t bs = HW_FLASH_BLOCK_MAX;
    uint32_t nblk, i, j, v = 0, rblen = 0;
    const uint8_t* rbody = NULL;
    int rc, ret = HW_FLASH_R_OK;

    if (!image || len == 0u) return HW_FLASH_R_BADSIZE;
    if (!g_bsp.uart_write && len > HW_FLASH_MODEL_CAP) return HW_FLASH_R_BADSIZE;

    memset(&g_sess, 0, sizeof(g_sess));
    g_sess.offset = offset; g_sess.image_size = len; g_sess.block_size = bs;
    g_stat.sessions++;

    rc = hw_flash_sync();
    if (rc != HW_FLASH_R_OK) { ret = rc; goto done; }

    if (hw_flash_read_reg((uint32_t)HW_FLASH_SPI_MAGIC_ADDR, &v) == HW_FLASH_R_OK)
        g_stat.chip_id = (uint8_t)v;

    nblk = (len + bs - 1u) / bs;
    begin[0]  = (uint8_t)(len & 0xFFu);   begin[1]  = (uint8_t)((len >> 8) & 0xFFu);
    begin[2]  = (uint8_t)((len >> 16) & 0xFFu); begin[3] = (uint8_t)((len >> 24) & 0xFFu);
    begin[4]  = (uint8_t)(nblk & 0xFFu);  begin[5]  = (uint8_t)((nblk >> 8) & 0xFFu);
    begin[6]  = (uint8_t)((nblk >> 16) & 0xFFu); begin[7] = (uint8_t)((nblk >> 24) & 0xFFu);
    begin[8]  = (uint8_t)(bs & 0xFFu);    begin[9]  = (uint8_t)((bs >> 8) & 0xFFu);
    begin[10] = 0u; begin[11] = 0u;
    begin[12] = (uint8_t)(offset & 0xFFu); begin[13] = (uint8_t)((offset >> 8) & 0xFFu);
    begin[14] = (uint8_t)((offset >> 16) & 0xFFu); begin[15] = (uint8_t)((offset >> 24) & 0xFFu);
    begin[16] = 0u; begin[17] = 0u; begin[18] = 0u; begin[19] = 0u;
    rc = fl_req(HW_FLASH_CMD_FLASH_BEGIN, begin, 20u, NULL, NULL, NULL);
    if (rc != HW_FLASH_R_OK) { ret = rc; goto done; }

    for (i = 0; i < nblk; i++) {
        uint32_t chunk = len - i * bs;
        if (chunk > bs) chunk = bs;
        body[0] = (uint8_t)(bs & 0xFFu);  body[1] = (uint8_t)((bs >> 8) & 0xFFu);
        body[2] = 0u; body[3] = 0u;
        body[4] = (uint8_t)(i & 0xFFu);   body[5] = (uint8_t)((i >> 8) & 0xFFu);
        body[6] = (uint8_t)((i >> 16) & 0xFFu); body[7] = (uint8_t)((i >> 24) & 0xFFu);
        for (j = 0; j < 8u; j++) body[8 + j] = 0u;
        for (j = 0; j < bs; j++)
            body[HW_FLASH_DATA_HDR + j] = (j < chunk) ? image[i * bs + j] : 0xFFu;

        rc = fl_req(HW_FLASH_CMD_FLASH_DATA, body, HW_FLASH_DATA_HDR + bs, NULL, NULL, NULL);
        if (rc != HW_FLASH_R_OK) { ret = rc; goto done; }
        g_sess.blocks_written++;
        g_sess.bytes_written += chunk;
        g_stat.bytes_flashed += chunk;
        g_sess.progress = (uint32_t)(((uint64_t)g_sess.bytes_written * 100u) / len);
        if (cb) cb(g_sess.progress, g_sess.bytes_written, ctx);
    }

    /* 本地 MD5 vs 设备回读 MD5 (32 字节 ASCII hex) */
    hw_flash_md5_hex(image, len, lhex);
    for (i = 0; i < 16u; i++) g_sess.md5_local[i] = (uint8_t)0;   /* 仅占位, 下面回填 */
    hw_flash_md5(image, len, g_sess.md5_local);
    md5b[0] = (uint8_t)(offset & 0xFFu);   md5b[1] = (uint8_t)((offset >> 8) & 0xFFu);
    md5b[2] = (uint8_t)((offset >> 16) & 0xFFu); md5b[3] = (uint8_t)((offset >> 24) & 0xFFu);
    md5b[4] = (uint8_t)(len & 0xFFu);      md5b[5] = (uint8_t)((len >> 8) & 0xFFu);
    md5b[6] = (uint8_t)((len >> 16) & 0xFFu); md5b[7] = (uint8_t)((len >> 24) & 0xFFu);
    rc = fl_req(HW_FLASH_CMD_FLASH_MD5, md5b, 8u, &v, &rbody, &rblen);
    if (rc != HW_FLASH_R_OK) { ret = rc; goto done; }
    if (rblen >= 32u && rbody) {
        uint32_t k;
        for (k = 0; k < 32u; k++) rhex[k] = (char)rbody[k];
        rhex[32] = '\0';
        for (k = 0; k < 16u; k++) {
            uint8_t hi = (uint8_t)((rbody[k*2] >= 'a') ? (rbody[k*2] - 'a' + 10) : (rbody[k*2] - '0'));
            uint8_t lo = (uint8_t)((rbody[k*2+1] >= 'a') ? (rbody[k*2+1] - 'a' + 10) : (rbody[k*2+1] - '0'));
            g_sess.md5_remote[k] = (uint8_t)((hi << 4) | lo);
        }
        for (k = 0; k < 32u; k++) if (rhex[k] != lhex[k]) break;
        g_sess.md5_match = (k == 32u) ? 1u : 0u;
    }
    g_stat.last_md5_ok = g_sess.md5_match;
    g_sess.done = 1u;
    g_sess.progress = 100u;
    ret = g_sess.md5_match ? HW_FLASH_R_OK : HW_FLASH_R_CHECKSUM;

done:
    if (out) *out = g_sess;
    fl_set_result(ret == HW_FLASH_R_OK ? "run ok: md5 verified" :
                  (ret == HW_FLASH_R_CHECKSUM ? "run fail: md5 mismatch" : "run fail"));
    return ret;
}

/* ============================================================
 * 10. 自检 (putf=NULL 静默)
 * ============================================================ */
/* 自检用传输桩 (必失败, 用于验证 IOERR 分支) */
static int fl_stub_fail_write(const uint8_t* d, uint32_t n) { (void)d; (void)n; return -1; }
static int fl_stub_fail_read(uint8_t* d, uint32_t cap) { (void)d; (void)cap; return -1; }

/* 自检用故障注入传输桩: 转发给模拟器, 但把每条 FLASH_DATA 的首个数据字节翻 1 bit,
 * 模拟「烧录链路单比特损坏」。设备存量 != 本地镜像 → MD5 校验必须报 CHECKSUM。
 * (uart_read 留空 → 回读仍走模拟器 g_rsp, 故校验逻辑完整参与) */
static int fl_stub_corrupt_write(const uint8_t* d, uint32_t n)
{
    /* static (真机加固): 合计 ~3.2KB; 该桩在 fl_req 的 fl_tx 内被回调,
     * 若与调用链上的栈缓冲叠加会踩爆 ESP32 小栈 */
    static uint8_t dec[HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX + 8u];
    static uint8_t enc[HW_FLASH_SLIP_CAP];
    int dl = hw_flash_slip_decode(d, n, dec, (uint32_t)sizeof(dec));
    if (dl < 0) return -1;
    if ((uint32_t)dl >= HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR &&
        dec[1] == (uint8_t)HW_FLASH_CMD_FLASH_DATA) {
        dec[HW_FLASH_HDR_LEN + HW_FLASH_DATA_HDR] ^= 0x01u;   /* 翻首数据字节 */
    }
    {
        int e = hw_flash_slip_encode(dec, (uint32_t)dl, enc, (uint32_t)sizeof(enc));
        if (e < 0) return -1;
        if (sim_write(enc, (uint32_t)e) < 0) return -1;
    }
    return (int)n;
}

#define FL_CHK(cond, msg) do { if (!(cond)) { fails++; if (putf) putf("  [FAIL] " msg "\n"); } \
                               else if (putf) putf("  [ok] " msg "\n"); } while (0)

int hw_flash_selftest(int (*putf)(const char*))
{
    int fails = 0;
    /* static (真机加固): 合计 ~16.4KB —— 真机 IDLE/小栈任务里必然栈溢出
     * (实测: 把整条 selftest→run→fl_req 链放栈上 ≈25KB → IDLE1 栈溢出复位) */
    static uint8_t img[4096];
    static uint8_t enc[8192 + 8];
    static uint8_t dec[4096];
    char buf[128];
    uint32_t i;
    int e, d, ok;
    uint32_t v = 0, rblen = 0;
    const uint8_t* rbody = NULL;
    hw_flash_session_t sess;
    hw_flash_stat_t st;

    hw_flash_init(NULL);
    if (putf) putf("hw_flash selftest (ESP32 ROM 下载协议烧录层)\n");

    /* 1. 模式 / 命令表黄金 */
    FL_CHK(hw_flash_mode() < HW_FLASH_MODE_MAX, "mode probe in range");
    FL_CHK(hw_flash_cmd_count() == 9u, "cmd table size == 9");
    FL_CHK(hw_flash_cmd_checksum() == HW_FLASH_GOLDEN, "cmd table FNV-1a-32 == golden");

    /* 2. SLIP 黄金向量: c0 db 00 dc dd 02 */
    {
        uint8_t vin[6];
        uint8_t w[10] = {0xC0,0xDB,0xDC,0xDB,0xDD,0x00,0xDC,0xDD,0x02,0xC0};
        vin[0]=0xC0; vin[1]=0xDB; vin[2]=0x00; vin[3]=0xDC; vin[4]=0xDD; vin[5]=0x02;
        e = hw_flash_slip_encode(vin, 6, enc, (uint32_t)sizeof(enc));
        ok = (e == 10);
        for (i = 0; i < 10u && ok; i++) if (enc[i] != w[i]) ok = 0;
        FL_CHK(ok, "slip encode golden vector C0/DB escape");
        d = hw_flash_slip_decode(enc, (uint32_t)e, dec, (uint32_t)sizeof(dec));
        ok = (d == 6);
        for (i = 0; i < 6u && ok; i++) if (dec[i] != vin[i]) ok = 0;
        FL_CHK(ok, "slip decode inverts escape");
    }

    /* 3. SLIP 回环 (4096B 含 C0/DB) */
    hw_flash_test_image(img, 4096);
    e = hw_flash_slip_encode(img, 4096, enc, (uint32_t)sizeof(enc));
    d = (e > 0) ? hw_flash_slip_decode(enc, (uint32_t)e, dec, (uint32_t)sizeof(dec)) : -1;
    ok = (d == 4096);
    if (ok) for (i = 0; i < 4096u; i++) if (dec[i] != img[i]) { ok = 0; break; }
    FL_CHK(ok, "slip roundtrip 4096B (59x C0 + 47x DB)");

    /* 4. XOR 校验和 + MD5 黄金 */
    FL_CHK(hw_flash_checksum(img, 1024) == 0xFDu, "xor checksum(img[0..1024]) == 0xFD");
    hw_flash_md5_hex(NULL, 0, buf);
    FL_CHK(strcmp(buf, "d41d8cd98f00b204e9800998ecf8427e") == 0, "md5(\"\") RFC1321");
    hw_flash_md5_hex((const uint8_t*)"abc", 3, buf);
    FL_CHK(strcmp(buf, "900150983cd24fb0d6963f7d28e17f72") == 0, "md5(\"abc\") RFC1321");
    hw_flash_md5_hex(img, 4096, buf);
    FL_CHK(strcmp(buf, HW_FLASH_IMG4096_MD5) == 0, "md5(4096B image) == golden");

    /* 5. 请求帧构造 */
    {
        uint8_t fr[64];
        uint8_t b4[4] = {0x10, 0x1F, 0x00, 0x60};
        int fl = hw_flash_frame_build(HW_FLASH_CMD_READ_REG, b4, 4, fr, (uint32_t)sizeof(fr));
        ok = (fl == 12) && fr[0] == 0x00 && fr[1] == 0x0A && fr[2] == 0x04 && fr[3] == 0x00;
        ok = ok && (fr[4] == hw_flash_checksum(b4, 4));
        ok = ok && fr[8] == 0x10 && fr[11] == 0x60;
        FL_CHK(ok, "frame build: dir/cmd/len/xor + body@8");
    }

    /* 6. 协议原语: SYNC / READ_REG / SEC_INFO */
    FL_CHK(hw_flash_sync() == HW_FLASH_R_OK, "SYNC handshake");
    FL_CHK(hw_flash_read_reg(HW_FLASH_SPI_MAGIC_ADDR, &v) == HW_FLASH_R_OK &&
           v == HW_FLASH_CHIP_ID, "READ_REG magic -> 9 (ESP32-S3)");
    FL_CHK(hw_flash_read_reg(HW_FLASH_SPI_CMD_ADDR, &v) == HW_FLASH_R_OK && v == 0u,
           "READ_REG SPI_CMD -> 0 (idle)");
    rblen = 0;
    FL_CHK(fl_req(HW_FLASH_CMD_SEC_INFO, NULL, 0, &v, &rbody, &rblen) == HW_FLASH_R_OK &&
           rblen == 20u && rbody && rbody[12] == (uint8_t)HW_FLASH_CHIP_ID,
           "SEC_INFO -> 20B, chip_id@12 == 9");
    /* RDID 语义: 未写 SPI_USR2 时 W0 = 0 */
    FL_CHK(hw_flash_read_reg(HW_FLASH_SPI_RDID_ADDR, &v) == HW_FLASH_R_OK && v == 0u,
           "READ_REG W0 -> 0 (no RDID yet)");

    /* 7. 端到端烧录 4096B -> MD5 校验 */
    {
        int rc = hw_flash_run(img, 4096, 0x10000u, &sess, NULL, NULL);
        FL_CHK(rc == HW_FLASH_R_OK, "end-to-end flash 4096B (md5 verified)");
        FL_CHK(sess.md5_match == 1u, "session md5_match == 1");
        FL_CHK(sess.blocks_written == 4u && sess.bytes_written == 4096u, "4 blocks x 1024B");
        FL_CHK(sess.progress == 100u, "progress == 100");
        hw_flash_stat(&st);
        FL_CHK(st.sessions >= 1u && st.chip_id == (uint8_t)HW_FLASH_CHIP_ID, "stat sessions/chip_id");
    }

    /* 8. 负向: 烧录链路单比特损坏 → MD5 必须报 CHECKSUM
     *   (注: 不能靠"烧前改镜像"——模拟器忠实存下所收内容, 两端仍一致;
     *    必须让"线上数据"被损坏, 才真正考验 MD5 校验路径) */
    {
        hw_flash_bsp_t cb;
        int rc;
        memset(&cb, 0, sizeof(cb));
        cb.uart_write = fl_stub_corrupt_write;   /* uart_read=NULL → 回读仍走模拟器 */
        hw_flash_bsp_install(&cb);
        rc = hw_flash_run(img, 4096, 0x10000u, &sess, NULL, NULL);
        hw_flash_bsp_install(NULL);
        FL_CHK(rc == HW_FLASH_R_CHECKSUM && sess.md5_match == 0u,
               "negative: 1-bit wire corruption -> CHECKSUM");
        FL_CHK(hw_flash_run(img, 4096, 0x10000u, &sess, NULL, NULL) == HW_FLASH_R_OK,
               "BSP uninstall restores clean flash");
    }

    /* 9. 负向: 坏传输 BSP -> IOERR */
    {
        hw_flash_bsp_t bad;
        int rc;
        memset(&bad, 0, sizeof(bad));
        bad.uart_write = NULL;  /* 无 write -> 走模拟器; 改用返回 -1 的桩 */
        bad.uart_write = fl_stub_fail_write;
        bad.uart_read  = fl_stub_fail_read;
        hw_flash_bsp_install(&bad);
        rc = hw_flash_sync();
        hw_flash_bsp_install(NULL);
        FL_CHK(rc == HW_FLASH_R_IOERR, "negative: bad BSP -> IOERR");
        FL_CHK(hw_flash_sync() == HW_FLASH_R_OK, "BSP uninstall restores simulator");
    }

    /* 10. 字符串助手 */
    FL_CHK(strcmp(hw_flash_mode_str(HW_FLASH_MODE_HOST), "HOST") == 0, "mode_str(HOST)");
    FL_CHK(strcmp(hw_flash_result_code_str(HW_FLASH_R_PROTO), "PROTO") == 0, "result_code_str(PROTO)");

    if (putf) {
        char line[64];
        snprintf(line, sizeof(line), "hw_flash selftest: %s (%d fails)\n",
                 fails == 0 ? "ALL PASS" : "FAILED", fails);
        putf(line);
    }
    return fails;
}

/* ============================================================
 * 11. 命令分发 (VM / hw_main) + CLI
 * ============================================================ */
static uint8_t s_img[HW_FLASH_MODEL_CAP];

/* CLI 输出适配 (与 hw_core 的 core_cli_puts 同款): 自检 putf 回调 */
static int flash_cli_puts(const char* s) { return printf("%s", s); }

static void hw_flash_card(void)
{
    uint8_t m = hw_flash_mode();
    printf("hw_flash - ESP32 ROM 下载协议烧录层 (全跨式六模式)\n");
    printf("  mode      : %s (%u)\n", hw_flash_mode_str(m), (unsigned)m);
    printf("  ROM cmds  : %u 条 (黄金 FNV-1a-32 = 0x%08X)\n",
           (unsigned)hw_flash_cmd_count(), (unsigned)hw_flash_cmd_checksum());
    printf("  block     : %u B (S3 ROM 无 stub)   model: %u B\n",
           (unsigned)HW_FLASH_BLOCK_MAX, (unsigned)HW_FLASH_MODEL_CAP);
    printf("  xport     : %s\n", g_bsp.uart_write ? "BSP (真机 UART)" : "simulator (确定性 ROM)");
    printf("---- ROM 命令表 ----\n");
    {
        uint32_t i;
        for (i = 0; i < hw_flash_cmd_count(); i++)
            printf("  0x%02X  %s\n", (unsigned)g_cmd_table[i].code,
                   fl_cmd_name(g_cmd_table[i].code));
    }
    printf("  cmds      : card|mode|slip|md5|sync|chip|run N|verify|stat|selftest\n");
}

static int fl_cmd_run(uint32_t n)
{
    hw_flash_session_t s;
    int rc;
    if (n == 0u) n = 4096u;
    if (n > HW_FLASH_MODEL_CAP) { printf("flash: size %u > model cap %u\n",
                                         (unsigned)n, (unsigned)HW_FLASH_MODEL_CAP); return HW_FLASH_R_BADSIZE; }
    hw_flash_test_image(s_img, n);
    printf("flash: 镜像 %u B (确定性模式镜像, 含 C0/DB 转义字节)\n", (unsigned)n);
    rc = hw_flash_run(s_img, n, 0x10000u, &s, NULL, NULL);
    if (rc == HW_FLASH_R_OK) {
        char hex[33];
        hw_flash_md5_hex(s_img, n, hex);
        printf("  %u blocks x %u B -> 100%%\n", (unsigned)s.blocks_written, (unsigned)s.block_size);
        printf("  local  md5 = %s\n", hex);
        printf("  device md5 = %s  (VERIFIED)\n", hex);
        printf("flash: OK\n");
    } else {
        printf("flash: FAIL rc=%d (%s)\n", rc, hw_flash_result_code_str(rc));
    }
    return rc;
}

int hw_flash_cmd(const char* cmd, void* ctx)
{
    (void)ctx;
    if (!cmd || cmd[0] == '\0' || strcmp(cmd, "help") == 0) {
        printf("hw_flash cmds: card|mode|slip|md5|sync|chip|run N|verify|stat|selftest\n");
        return HW_FLASH_R_HELP;
    }
    if (strcmp(cmd, "card") == 0) { hw_flash_card(); return HW_FLASH_R_OK; }
    if (strcmp(cmd, "mode") == 0) {
        printf("hw_flash mode: %s (%u)\n", hw_flash_mode_str(hw_flash_mode()), (unsigned)hw_flash_mode());
        return HW_FLASH_R_OK;
    }
    if (strcmp(cmd, "slip") == 0) {
        uint8_t vin[6]; uint8_t enc[32]; uint8_t dec[32];
        int e, d, i, ok;
        vin[0]=0xC0; vin[1]=0xDB; vin[2]=0x00; vin[3]=0xDC; vin[4]=0xDD; vin[5]=0x02;
        e = hw_flash_slip_encode(vin, 6, enc, (uint32_t)sizeof(enc));
        d = hw_flash_slip_decode(enc, (uint32_t)e, dec, (uint32_t)sizeof(dec));
        ok = (d == 6);
        for (i = 0; i < 6 && ok; i++) if (dec[i] != vin[i]) ok = 0;
        printf("slip in : %02x %02x %02x %02x %02x %02x\n", vin[0],vin[1],vin[2],vin[3],vin[4],vin[5]);
        printf("slip out:");
        for (i = 0; i < e; i++) printf(" %02x", enc[i]);
        printf("   (%d B, roundtrip %s)\n", e, ok ? "OK" : "FAIL");
        return ok ? HW_FLASH_R_OK : HW_FLASH_R_PROTO;
    }
    if (strcmp(cmd, "md5") == 0) {
        char hex[33];
        hw_flash_test_image(s_img, 4096);
        hw_flash_md5_hex(s_img, 4096, hex);
        printf("md5(4096B test image) = %s\n", hex);
        printf("golden                = %s  (%s)\n", HW_FLASH_IMG4096_MD5,
               strcmp(hex, HW_FLASH_IMG4096_MD5) == 0 ? "MATCH" : "MISMATCH");
        return HW_FLASH_R_OK;
    }
    if (strcmp(cmd, "sync") == 0) {
        int rc = hw_flash_sync();
        printf("flash: SYNC -> %s\n", hw_flash_result_code_str(rc));
        return rc;
    }
    if (strcmp(cmd, "chip") == 0) {
        uint32_t v = 0;
        int rc = hw_flash_read_reg((uint32_t)HW_FLASH_SPI_MAGIC_ADDR, &v);
        if (rc == HW_FLASH_R_OK) {
            g_stat.chip_id = (uint8_t)v;
            printf("flash: chip magic = 0x%08X -> %s\n", (unsigned)v,
                   (v == HW_FLASH_CHIP_ID) ? "ESP32-S3" : "unknown");
        } else printf("flash: chip probe FAIL rc=%d\n", rc);
        return rc;
    }
    if (strncmp(cmd, "run", 3) == 0) {
        uint32_t n = (cmd[3] != '\0') ? (uint32_t)strtol(cmd + 3, NULL, 0) : 0u;
        return fl_cmd_run(n);
    }
    if (strcmp(cmd, "verify") == 0) {
        uint32_t v = 0, rblen = 0;
        const uint8_t* rbody = NULL;
        int rc = fl_req(HW_FLASH_CMD_FLASH_MD5, NULL, 0, &v, &rbody, &rblen);
        printf("flash: verify -> rc=%d md5_match=%u\n", rc, (unsigned)g_sess.md5_match);
        return rc;
    }
    if (strcmp(cmd, "stat") == 0) {
        hw_flash_stat_t st;
        hw_flash_stat(&st);
        printf("flash stat: sessions=%u tx=%u rx=%u flashed=%u B md5_ok=%u chip_id=%u\n",
               (unsigned)st.sessions, (unsigned)st.cmd_tx, (unsigned)st.cmd_rx,
               (unsigned)st.bytes_flashed, (unsigned)st.last_md5_ok, (unsigned)st.chip_id);
        return HW_FLASH_R_OK;
    }
    if (strcmp(cmd, "selftest") == 0) {
        int f = hw_flash_selftest(flash_cli_puts);
        return (f == 0) ? HW_FLASH_R_OK : HW_FLASH_R_PROTO;
    }
    return HW_FLASH_R_NOCMD;
}

int hw_flash_cli(int argc, char** argv)
{
    /* 家族约定: argv[1]="flash", 命令串在 argv[2] (argc>=3); 缺省 = card。
     * ⚠️ 上一轮误用 argv[1] → 永远取到 "flash" → 全分支不匹配 → exit 255 零输出。 */
    const char* sub = (argc >= 3 && argv[2]) ? argv[2] : "card";
    int rc;

    hw_flash_init(NULL);            /* CLI 一次性路径也须有干净现场可读 */

    if (strcmp(sub, "selftest") == 0) {
        int fails = hw_flash_selftest(flash_cli_puts);
        if (fails != 0) printf("selftest: %d 项失败\n", fails);
        return (fails == 0) ? 0 : 1;
    }
    if (strcmp(sub, "help") == 0) {
        printf("hw_flash cmds: card|mode|slip|md5|sync|chip|run N|verify|stat|selftest\n");
        return 0;
    }

    rc = hw_flash_cmd(sub, NULL);
    /* rc >= 0: 结果码 0=OK 即成功; rc < 0: -1 未识别 / -2 help → 归一化为 exit 1 */
    return (rc >= 0) ? rc : 1;
}
