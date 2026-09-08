/*
 * t48_usb.c - XGecu T48 烧录器·自研驱动 USB 通信层（libusb 实现）
 *
 * 通过 libusb-1.0 的批量传输与 T48 通信。协议事实逆向自 T48 设备行为，
 * 端点/分片规则对拍 minipro 的 usb_nix.c 思路（独立实现，未复制代码）。
 */

#include "t48_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

struct t48_usb {
    libusb_context *ctx;
    libusb_device_handle *dev;
    int verbose;
};

static const char *_errname(int ret)
{
    switch (ret) {
    case LIBUSB_SUCCESS: return "OK";
    case LIBUSB_ERROR_IO: return "IO";
    case LIBUSB_ERROR_INVALID_PARAM: return "INVALID_PARAM";
    case LIBUSB_ERROR_ACCESS: return "ACCESS (macOS 需授权终端访问 USB)";
    case LIBUSB_ERROR_NO_DEVICE: return "NO_DEVICE";
    case LIBUSB_ERROR_NOT_FOUND: return "NOT_FOUND";
    case LIBUSB_ERROR_BUSY: return "BUSY (被其它进程占用)";
    case LIBUSB_ERROR_TIMEOUT: return "TIMEOUT";
    case LIBUSB_ERROR_OVERFLOW: return "OVERFLOW";
    case LIBUSB_ERROR_PIPE: return "PIPE";
    case LIBUSB_ERROR_INTERRUPTED: return "INTERRUPTED";
    default: return "OTHER";
    }
}

const char *t48_usb_strerror(int ret) { return _errname(ret); }

static void hexdump(const char *tag, const uint8_t *d, size_t n)
{
    fprintf(stderr, "%s (%zuB)\n", tag, n);
    for (size_t i = 0; i < n; i += 16) {
        fprintf(stderr, "  %04zx  ", i);
        for (size_t j = 0; j < 16 && i + j < n; j++)
            fprintf(stderr, "%02x ", d[i + j]);
        fprintf(stderr, "\n");
    }
}

/* 单路 bulk 传输。返回 0 成功，*transferred 为实际字节数。 */
static int bulk(t48_usb_t *u, uint8_t ep, uint8_t *buf, size_t len,
                int *transferred, unsigned timeout)
{
    int ret = libusb_bulk_transfer(u->dev, ep, buf, (int)len, transferred,
                                   timeout);
    if (ret != 0 && u->verbose)
        fprintf(stderr, "bulk EP%02x: %s\n", ep, _errname(ret));
    return ret;
}

int t48_msg_send(t48_usb_t *u, const uint8_t *data, size_t n)
{
    int xfer = 0;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) return LIBUSB_ERROR_NO_MEM;
    memcpy(buf, data, n);
    int ret = bulk(u, T48_EP_CMD_OUT, buf, n, &xfer, T48_USB_TIMEOUT);
    free(buf);
    if (ret == 0 && xfer != (int)n) {
        if (u->verbose)
            fprintf(stderr, "短写 %d/%zu\n", xfer, n);
        return LIBUSB_ERROR_IO;
    }
    return ret;
}

int t48_msg_recv(t48_usb_t *u, uint8_t *data, size_t n)
{
    int xfer = 0;
    uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
    if (!buf) return LIBUSB_ERROR_NO_MEM;
    int ret = bulk(u, T48_EP_CMD_IN, buf, n, &xfer, T48_USB_READ_TIMEOUT);
    if (ret == 0) memcpy(data, buf, xfer);
    free(buf);
    return ret;
}

/* 写载荷：<=limit 走 EP2 单发；>limit 拆两半 EP2+EP3。 */
int t48_write_payload(t48_usb_t *u, const uint8_t *data, size_t n, size_t limit)
{
    int xfer = 0;
    if (n <= limit) {
        uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
        if (!buf) return LIBUSB_ERROR_NO_MEM;
        memcpy(buf, data, n);
        int ret = bulk(u, T48_EP_DATA_OUT_A, buf, n, &xfer, T48_USB_TIMEOUT);
        free(buf);
        return ret;
    }
    /* 分片（对拍 XgPro 规则）：按 128 对齐算 EP2/EP3 长度 */
    size_t j = n % 128;
    size_t ep2_len, ep3_len;
    if (j) {
        size_t k = (n - j) / 2;
        if (j > 64) { ep2_len = k + 64; ep3_len = j + k - 64; }
        else        { ep2_len = k;      ep3_len = j + k; }
    } else {
        ep3_len = n / 2;
        ep2_len = ep3_len;
    }
    uint8_t *b2 = (uint8_t *)malloc(ep2_len ? ep2_len : 1);
    uint8_t *b3 = (uint8_t *)malloc(ep3_len ? ep3_len : 1);
    if (!b2 || !b3) { free(b2); free(b3); return LIBUSB_ERROR_NO_MEM; }
    memcpy(b2, data, ep2_len);
    memcpy(b3, data + ep2_len, ep3_len);
    int r2 = bulk(u, T48_EP_DATA_OUT_A, b2, ep2_len, &xfer, T48_USB_TIMEOUT);
    int r3 = bulk(u, T48_EP_DATA_OUT_B, b3, ep3_len, &xfer, T48_USB_TIMEOUT);
    free(b2); free(b3);
    return r2 ? r2 : r3;
}

/* 读载荷：<64 读 64 取头部；==64 走 EP2；>64 拆两半 EP2+EP3 去交错。 */
int t48_read_payload(t48_usb_t *u, uint8_t *data, size_t n)
{
    int xfer = 0;
    if (n < 64) {
        uint8_t buf[64];
        int ret = bulk(u, T48_EP_DATA_IN_A, buf, 64, &xfer, T48_USB_TIMEOUT);
        if (ret == 0) memcpy(data, buf, n);
        return ret;
    }
    if (n == 64)
        return bulk(u, T48_EP_DATA_IN_A, data, n, &xfer, T48_USB_TIMEOUT);

    size_t half = n / 2;
    uint8_t *e2 = (uint8_t *)malloc(half);
    uint8_t *e3 = (uint8_t *)malloc(half);
    if (!e2 || !e3) { free(e2); free(e3); return LIBUSB_ERROR_NO_MEM; }
    int r2 = bulk(u, T48_EP_DATA_IN_A, e2, half, &xfer, T48_USB_TIMEOUT);
    int r3 = bulk(u, T48_EP_DATA_IN_B, e3, half, &xfer, T48_USB_TIMEOUT);
    /* 去交错：每 64B 块轮流来自 EP2/EP3 */
    size_t blocks = n / 64;
    for (size_t i = 0; i < blocks; i++) {
        const uint8_t *src = (i % 2 == 0) ? e2 : e3;
        memcpy(data + i * 64, src + (i / 2) * 64, 64);
    }
    size_t tail = n % 64;
    if (tail) {
        const uint8_t *src = (blocks % 2 == 0) ? e2 : e3;
        memcpy(data + blocks * 64, src + (blocks / 2) * 64, tail);
    }
    free(e2); free(e3);
    return r2 ? r2 : r3;
}

int t48_usb_open(t48_usb_t **out, int verbose)
{
    t48_usb_t *u = (t48_usb_t *)calloc(1, sizeof(*u));
    if (!u) return LIBUSB_ERROR_NO_MEM;
    u->verbose = verbose;

    int ret = libusb_init(&u->ctx);
    if (ret != 0) { free(u); return ret; }

    u->dev = libusb_open_device_with_vid_pid(u->ctx, T48_VID, T48_PID);
    if (!u->dev) {
        if (verbose)
            fprintf(stderr, "未找到 T48 (VID=%04x PID=%04x)\n", T48_VID, T48_PID);
        libusb_exit(u->ctx);
        free(u);
        return LIBUSB_ERROR_NOT_FOUND;
    }

    ret = libusb_claim_interface(u->dev, 0);
    if (ret != 0) {
        if (verbose)
            fprintf(stderr, "claim 接口失败: %s\n", _errname(ret));
        libusb_close(u->dev);
        libusb_exit(u->ctx);
        free(u);
        return ret;
    }

    *out = u;
    return 0;
}

void t48_usb_close(t48_usb_t *u)
{
    if (!u) return;
    if (u->dev) {
        libusb_release_interface(u->dev, 0);
        libusb_close(u->dev);
    }
    if (u->ctx) libusb_exit(u->ctx);
    free(u);
}

int t48_usb_probe(int verbose)
{
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 0;
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int count = 0;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) == 0 &&
            d.idVendor == T48_VID && d.idProduct == T48_PID)
            count++;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    if (verbose) {
        printf(count ? "✅ 发现 T48: VID=%04x PID=%04x x%d\n" : "❌ 未发现 T48\n",
               T48_VID, T48_PID, count);
    }
    return count;
}
