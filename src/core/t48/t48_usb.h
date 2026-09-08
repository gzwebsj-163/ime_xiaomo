/*
 * t48_usb.h - XGecu T48 烧录器·自研驱动 USB 通信层
 *
 * 基于逆向出的 T48 协议事实实现（clean-room）：
 *   VID=0xa466 PID=0x0a53（TL866II+/T48/T56 共用）
 *   EP 0x01 OUT 命令 / 0x81 IN 响应
 *   EP 0x02/0x03 OUT 载荷 / 0x82/0x83 IN 载荷（大数据分片+去交错）
 */

#ifndef T48_USB_H
#define T48_USB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T48_VID 0xa466
#define T48_PID 0x0a53

#define T48_EP_CMD_OUT  0x01
#define T48_EP_CMD_IN   0x81
#define T48_EP_DATA_OUT_A 0x02
#define T48_EP_DATA_IN_A  0x82
#define T48_EP_DATA_OUT_B 0x03
#define T48_EP_DATA_IN_B  0x83

#define T48_USB_TIMEOUT      5000     /* ms */
#define T48_USB_READ_TIMEOUT 360000   /* ms：读响应等得久 */

typedef struct t48_usb t48_usb_t;

/* 打开 T48（init + open + claim interface 0）。成功返回 0。 */
int  t48_usb_open(t48_usb_t **out, int verbose);
void t48_usb_close(t48_usb_t *u);

/* 枚举：返回在线 T48 数量（不打开）。 */
int  t48_usb_probe(int verbose);

/* 命令消息（EP01 OUT / EP81 IN） */
int  t48_msg_send(t48_usb_t *u, const uint8_t *data, size_t n);
int  t48_msg_recv(t48_usb_t *u, uint8_t *data, size_t n);

/* 载荷传输（EP2/EP3 分片） */
int  t48_write_payload(t48_usb_t *u, const uint8_t *data, size_t n, size_t limit);
int  t48_read_payload(t48_usb_t *u, uint8_t *data, size_t n);

const char *t48_usb_strerror(int ret);

#ifdef __cplusplus
}
#endif

#endif /* T48_USB_H */
