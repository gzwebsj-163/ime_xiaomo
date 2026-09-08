/*
 * t48_proto.c - XGecu T48 烧录器·自研驱动 命令协议层实现
 *
 * 实现 T48 协议命令：BEGIN_TRANS/END_TRANS/READ_CODE/WRITE_CODE/ERASE/
 * READID/AUTODETECT/PROTECT_OFF/ON/REQUEST_STATUS
 *
 * 协议格式（对拍 minipro t48.c）：
 *   命令消息：64 字节，msg[0]=命令码，后续参数按小端序填充
 *   载荷传输：EP02/EP03 分片+去交错（t48_usb 层处理）
 */

#include "t48_proto.h"
#include "t48_usb.h"
#include "t48_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== 命令码（与 T48 固件协议对齐） ========== */
#define CMD_BEGIN_TRANS     0x03
#define CMD_END_TRANS       0x04
#define CMD_READID          0x05
#define CMD_READ_USER       0x06
#define CMD_WRITE_USER      0x07
#define CMD_READ_CFG        0x08
#define CMD_WRITE_CFG       0x09
#define CMD_WRITE_USER_DATA 0x0A
#define CMD_READ_USER_DATA  0x0B
#define CMD_WRITE_CODE      0x0C
#define CMD_READ_CODE       0x0D
#define CMD_ERASE           0x0E
#define CMD_READ_DATA       0x10
#define CMD_WRITE_DATA      0x11
#define CMD_WRITE_LOCK      0x14
#define CMD_READ_LOCK       0x15
#define CMD_PROTECT_OFF     0x18
#define CMD_PROTECT_ON      0x19
#define CMD_AUTODETECT      0x37
#define CMD_REQUEST_STATUS  0x39

/* 小端写入 */
static void le16(uint8_t *buf, uint16_t v)
{
    buf[0] = (uint8_t)(v & 0xff);
    buf[1] = (uint8_t)((v >> 8) & 0xff);
}

static void le32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v & 0xff);
    buf[1] = (uint8_t)((v >> 8) & 0xff);
    buf[2] = (uint8_t)((v >> 16) & 0xff);
    buf[3] = (uint8_t)((v >> 24) & 0xff);
}

/* 大端读取 */
static uint32_t be24(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
}

/* ========== 句柄结构 ========== */
struct t48_handle {
    t48_usb_t *usb;
    const t48_device_t *dev;  /* 选中芯片，NULL=未选 */
    char chip_name[40];
    int verbose;
    int in_transaction;       /* 是否在事务中 */
};

/* ========== 生命周期 ========== */

t48_handle_t *t48_open(const char *chip_name, int verbose)
{
    t48_handle_t *h = (t48_handle_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->verbose = verbose;

    /* 打开 USB */
    int ret = t48_usb_open(&h->usb, verbose);
    if (ret != 0) {
        if (verbose)
            fprintf(stderr, "❌ T48 打开失败: %s\n", t48_usb_strerror(ret));
        free(h);
        return NULL;
    }

    /* 查找芯片 */
    if (chip_name) {
        h->dev = t48_db_find(chip_name);
        if (!h->dev) {
            if (verbose)
                fprintf(stderr, "❌ 未知芯片: %s\n", chip_name);
            t48_usb_close(h->usb);
            free(h);
            return NULL;
        }
        strncpy(h->chip_name, h->dev->name, sizeof(h->chip_name) - 1);
        if (verbose)
            printf("✅ 选中芯片: %s (%u bytes)\n",
                   h->dev->name, h->dev->code_memory_size);
    }

    return h;
}

void t48_close(t48_handle_t *h)
{
    if (!h) return;
    if (h->in_transaction)
        t48_end_transaction(h);
    t48_usb_close(h->usb);
    free(h);
}

int t48_probe(int verbose)
{
    return t48_usb_probe(verbose);
}

/* ========== 芯片操作 ========== */

int t48_begin_transaction(t48_handle_t *h)
{
    if (!h || !h->dev) return T48_ERR_PARAM;
    if (h->in_transaction) return T48_OK;  /* 已开始 */

    uint8_t msg[64];
    memset(msg, 0, sizeof(msg));

    const t48_device_t *d = h->dev;

    msg[0] = CMD_BEGIN_TRANS;
    msg[1] = d->protocol_id;          /* 协议 ID */
    msg[2] = (uint8_t)(d->variant & 0xff);  /* 变体低 8 位 */
    msg[3] = 0;                       /* ICSP = 0 */

    /* 电压编码 */
    le16(&msg[4], (uint16_t)(d->voltages & 0xffff));
    msg[6] = d->chip_info;
    msg[7] = (uint8_t)(d->pin_map & 0xff);

    /* 数据区大小 / 页大小 / 脉冲延迟 */
    le16(&msg[8],  (uint16_t)(d->data_memory_size & 0xffff));
    le16(&msg[10], d->page_size);
    le16(&msg[12], d->pulse_delay);

    /* 额外数据区大小（SPI flash 通常为 0） */
    le16(&msg[14], 0);

    /* 主区大小（code_memory_size） */
    le32(&msg[16], d->code_memory_size);

    /* 电压高位 */
    msg[20] = (uint8_t)((d->voltages >> 16) & 0xff);
    msg[22] = (uint8_t)(d->voltages & 0xf0);

    /* SPI 时钟 */
    if (d->spi_clock) {
        msg[24] = 1;       /* 启用时钟设置 */
        msg[28] = d->spi_clock;
    }

    /* 封装信息 */
    le32(&msg[40], d->package_details);
    le16(&msg[44], d->read_buffer_size);

    /* 标志位 */
    le32(&msg[56], d->flags);

    if (h->verbose > 1) {
        fprintf(stderr, "[T48] BEGIN_TRANS: proto=%02x var=%04x "
                "size=%u page=%u\n",
                d->protocol_id, d->variant,
                d->code_memory_size, d->page_size);
    }

    /* 发送命令 */
    int ret = t48_msg_send(h->usb, msg, 64);
    if (ret != 0) return T48_ERR_PROTO;

    /* 检查过流 */
    uint8_t ovc = 0;
    ret = t48_get_status(h, &ovc);
    if (ret != 0) return ret;
    if (ovc) {
        if (h->verbose)
            fprintf(stderr, "❌ 过流保护! 检查芯片引脚是否短路\n");
        return T48_ERR_OVC;
    }

    h->in_transaction = 1;
    return T48_OK;
}

int t48_end_transaction(t48_handle_t *h)
{
    if (!h || !h->in_transaction) return T48_OK;

    uint8_t msg[8] = { 0 };
    msg[0] = CMD_END_TRANS;
    int ret = t48_msg_send(h->usb, msg, 8);
    h->in_transaction = 0;
    return ret == 0 ? T48_OK : T48_ERR_PROTO;
}

int t48_read_chip_id(t48_handle_t *h, uint32_t *out_id)
{
    if (!h || !h->dev) return T48_ERR_PARAM;

    uint8_t msg[32];
    memset(msg, 0, sizeof(msg));
    msg[0] = CMD_READID;

    int ret = t48_msg_send(h->usb, msg, 8);
    if (ret != 0) return T48_ERR_PROTO;

    ret = t48_msg_recv(h->usb, msg, 32);
    if (ret != 0) return T48_ERR_PROTO;

    uint8_t id_type = msg[0];
    uint8_t id_len = h->dev->chip_id_bytes_count;
    if (id_len > 4) id_len = 4;

    if (id_len > 0) {
        /* 大端或小端取决于 ID 类型 */
        if (id_type == 3 || id_type == 4)
            *out_id = (uint32_t)msg[2] | ((uint32_t)msg[3] << 8) |
                      ((uint32_t)msg[4] << 16) | ((uint32_t)msg[5] << 24);
        else
            *out_id = ((uint32_t)msg[2] << 24) | ((uint32_t)msg[3] << 16) |
                      ((uint32_t)msg[4] << 8) | msg[5];
    } else {
        *out_id = 0;
    }

    if (h->verbose)
        printf("🔍 芯片 ID: type=%d id=0x%06x\n", id_type, *out_id & 0xffffff);

    return T48_OK;
}

int t48_spi_autodetect(t48_handle_t *h, uint32_t *out_id)
{
    if (!h) return T48_ERR_PARAM;

    uint8_t msg[64];
    memset(msg, 0, sizeof(msg));
    msg[0] = CMD_AUTODETECT;
    msg[8] = 0x03;  /* SPI NOR Flash 类型 */

    int ret = t48_msg_send(h->usb, msg, 10);
    if (ret != 0) return T48_ERR_PROTO;

    ret = t48_msg_recv(h->usb, msg, 32);
    if (ret != 0) return T48_ERR_PROTO;

    *out_id = be24(&msg[2]);

    if (h->verbose)
        printf("🔍 SPI 自动检测: JEDEC ID = 0x%06x\n", *out_id);

    /* 自动匹配芯片 */
    const t48_device_t *found = t48_db_find_by_id(*out_id);
    if (found && !h->dev) {
        h->dev = found;
        strncpy(h->chip_name, found->name, sizeof(h->chip_name) - 1);
        if (h->verbose)
            printf("✅ 自动匹配芯片: %s (%u bytes)\n",
                   found->name, found->code_memory_size);
    }

    return T48_OK;
}

int t48_read(t48_handle_t *h, int mem_type,
             uint32_t addr, uint8_t *data, size_t size)
{
    if (!h || !h->dev || !data) return T48_ERR_PARAM;
    if (size == 0) return T48_OK;

    uint8_t msg[64] = { 0 };

    /* 选择命令码 */
    switch (mem_type) {
    case T48_MEM_CODE: msg[0] = CMD_READ_CODE;  break;
    case T48_MEM_DATA: msg[0] = CMD_READ_DATA;  break;
    case T48_MEM_USER: msg[0] = CMD_READ_USER_DATA; break;
    default: return T48_ERR_PARAM;
    }

    le16(&msg[2], (uint16_t)(size & 0xffff));
    le32(&msg[4], addr);

    if (h->verbose > 1)
        fprintf(stderr, "[T48] READ: type=%d addr=0x%x size=%zu\n",
                mem_type, addr, size);

    int ret = t48_msg_send(h->usb, msg, 8);
    if (ret != 0) return T48_ERR_PROTO;

    ret = t48_read_payload(h->usb, data, size);
    return ret == 0 ? T48_OK : T48_ERR_IO;
}

int t48_write(t48_handle_t *h, int mem_type,
              uint32_t addr, const uint8_t *data, size_t size)
{
    if (!h || !h->dev || !data) return T48_ERR_PARAM;
    if (size == 0) return T48_OK;

    uint8_t msg[64] = { 0 };

    /* 选择命令码 */
    switch (mem_type) {
    case T48_MEM_CODE: msg[0] = CMD_WRITE_CODE;  break;
    case T48_MEM_DATA: msg[0] = CMD_WRITE_DATA;  break;
    case T48_MEM_USER: msg[0] = CMD_WRITE_USER_DATA; break;
    default: return T48_ERR_PARAM;
    }

    le16(&msg[2], (uint16_t)(size & 0xffff));
    le32(&msg[4], addr);

    if (h->verbose > 1)
        fprintf(stderr, "[T48] WRITE: type=%d addr=0x%x size=%zu\n",
                mem_type, addr, size);

    int ret = t48_msg_send(h->usb, msg, 8);
    if (ret != 0) return T48_ERR_PROTO;

    /* 载荷大小受写缓冲限制 */
    size_t limit = h->dev->write_buffer_size;
    ret = t48_write_payload(h->usb, data, size, limit);
    return ret == 0 ? T48_OK : T48_ERR_IO;
}

int t48_erase_chip(t48_handle_t *h)
{
    if (!h || !h->dev) return T48_ERR_PARAM;

    uint8_t msg[64];
    memset(msg, 0, sizeof(msg));
    msg[0] = CMD_ERASE;
    /* msg[2] = num_fuses (0=不擦 fuse) */
    /* msg[4] = pld (0) */

    if (h->verbose)
        printf("🧹 擦除中...\n");

    int ret = t48_msg_send(h->usb, msg, 15);
    if (ret != 0) return T48_ERR_PROTO;

    /* 擦除需要等待响应 */
    memset(msg, 0, sizeof(msg));
    ret = t48_msg_recv(h->usb, msg, 64);
    if (ret != 0) return T48_ERR_PROTO;

    if (h->verbose)
        printf("✅ 擦除完成\n");

    return T48_OK;
}

int t48_protect_off(t48_handle_t *h)
{
    if (!h) return T48_ERR_PARAM;
    uint8_t msg[8] = { 0 };
    msg[0] = CMD_PROTECT_OFF;
    int ret = t48_msg_send(h->usb, msg, 8);
    return ret == 0 ? T48_OK : T48_ERR_PROTO;
}

int t48_protect_on(t48_handle_t *h)
{
    if (!h) return T48_ERR_PARAM;
    uint8_t msg[8] = { 0 };
    msg[0] = CMD_PROTECT_ON;
    int ret = t48_msg_send(h->usb, msg, 8);
    return ret == 0 ? T48_OK : T48_ERR_PROTO;
}

int t48_get_status(t48_handle_t *h, uint8_t *ovc_out)
{
    if (!h) return T48_ERR_PARAM;

    uint8_t msg[32];
    memset(msg, 0, sizeof(msg));
    msg[0] = CMD_REQUEST_STATUS;

    int ret = t48_msg_send(h->usb, msg, 8);
    if (ret != 0) return T48_ERR_PROTO;

    ret = t48_msg_recv(h->usb, msg, 32);
    if (ret != 0) return T48_ERR_PROTO;

    if (ovc_out)
        *ovc_out = msg[12];

    return T48_OK;
}

/* ========== 高级操作 ========== */

int t48_read_all(t48_handle_t *h, uint8_t *data, t48_progress_fn progress)
{
    if (!h || !h->dev || !data) return T48_ERR_PARAM;

    uint32_t total = h->dev->code_memory_size;
    uint32_t block_size = h->dev->read_buffer_size;
    if (block_size == 0 || block_size > 0x10000) block_size = 0x1000;

    if (progress) progress(0, "开始读取");

    for (uint32_t addr = 0; addr < total; ) {
        uint32_t len = (total - addr < block_size) ? (total - addr) : block_size;
        int ret = t48_read(h, T48_MEM_CODE, addr, data + addr, len);
        if (ret != 0) {
            if (progress) progress(-1, "读取错误");
            return ret;
        }
        addr += len;
        if (progress) progress((int)(addr * 100 / total), NULL);
    }

    if (progress) progress(100, "读取完成");
    return (int)total;
}

int t48_write_all(t48_handle_t *h, const uint8_t *data, size_t size,
                  t48_progress_fn progress)
{
    if (!h || !h->dev || !data) return T48_ERR_PARAM;

    uint32_t total = h->dev->code_memory_size;
    if (size > total) size = total;

    /* 先擦除 */
    if (progress) progress(0, "擦除中...");
    int ret = t48_erase_chip(h);
    if (ret != 0) return ret;

    /* 分块写入 */
    uint32_t block_size = h->dev->write_buffer_size;
    if (block_size == 0 || block_size > 0x1000) block_size = 0x100;

    if (progress) progress(0, "开始写入");

    for (uint32_t addr = 0; addr < size; ) {
        uint32_t len = (uint32_t)(size - addr < block_size ? size - addr : block_size);
        ret = t48_write(h, T48_MEM_CODE, addr, data + addr, len);
        if (ret != 0) {
            if (progress) progress(-1, "写入错误");
            return ret;
        }
        addr += len;
        if (progress) progress((int)(addr * 100 / total), NULL);
    }

    if (progress) progress(100, "写入完成");
    return T48_OK;
}

int t48_verify(t48_handle_t *h, const uint8_t *data, size_t size,
               t48_progress_fn progress)
{
    if (!h || !h->dev || !data) return T48_ERR_PARAM;

    uint32_t total = h->dev->code_memory_size;
    if (size > total) size = total;
    uint32_t block_size = h->dev->read_buffer_size;
    if (block_size == 0 || block_size > 0x10000) block_size = 0x1000;

    uint8_t *buf = (uint8_t *)malloc(block_size);
    if (!buf) return T48_ERR_MEM;

    if (progress) progress(0, "开始校验");

    int result = 0;
    for (uint32_t addr = 0; addr < size && result == 0; ) {
        uint32_t len = (uint32_t)(size - addr < block_size ? size - addr : block_size);
        int ret = t48_read(h, T48_MEM_CODE, addr, buf, len);
        if (ret != 0) {
            free(buf);
            if (progress) progress(-1, "校验读错误");
            return ret;
        }
        if (memcmp(buf, data + addr, len) != 0) {
            /* 找到第一个不一致的偏移 */
            for (uint32_t i = 0; i < len; i++) {
                if (buf[i] != data[addr + i]) {
                    result = (int)(addr + i) + 1;  /* 1-based 偏移 */
                    break;
                }
            }
        }
        addr += len;
        if (progress) progress((int)(addr * 100 / total), NULL);
    }

    free(buf);
    if (progress) progress(100, result ? "校验失败" : "校验通过");
    return result;
}

/* ========== 辅助方法 ========== */

const char *t48_chip_name(t48_handle_t *h)
{
    return h ? h->chip_name : NULL;
}

uint32_t t48_chip_size(t48_handle_t *h)
{
    return (h && h->dev) ? h->dev->code_memory_size : 0;
}

const char *t48_strerror(int err)
{
    switch (err) {
    case T48_OK:          return "成功";
    case T48_ERR_OPEN:    return "打开失败";
    case T48_ERR_PROTO:   return "协议错误";
    case T48_ERR_OVC:     return "过流保护";
    case T48_ERR_CHIPID:  return "芯片 ID 不匹配";
    case T48_ERR_MEM:     return "内存不足";
    case T48_ERR_PARAM:   return "参数错误";
    case T48_ERR_IO:      return "USB I/O 错误";
    default:              return "未知错误";
    }
}