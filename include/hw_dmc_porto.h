#ifndef DMC_PROTO_H
#define DMC_PROTO_H
#include "hw_dmc.h"

// DMC 命令定义
#define DMC_CMD_HELLO        0x01
#define DMC_CMD_HELLO_ACK    0x02
#define DMC_CMD_DATA         0x03
#define DMC_CMD_DATA_ACK     0x04
#define DMC_CMD_RESET        0x05
#define DMC_CMD_RESET_ACK    0x06
#define DMC_CMD_STATUS       0x07
#define DMC_CMD_STATUS_RSP   0x08
#define DMC_CMD_NACK         0x0F

// DMC状态
typedef enum {
    DMC_STATE_IDLE,
    DMC_STATE_SEND_HELLO,
    DMC_STATE_WAIT_HELLO_ACK,
    DMC_STATE_ESTABLISHED,
    DMC_STATE_ERROR
} DmcState_t;

// DMC帧结构体
typedef struct {
    uint8_t  sync0;
    uint8_t  sync1;
    uint8_t  len;
    uint8_t  cmd;
    uint8_t  payload[248];
    uint16_t crc16;
} DmcFrame_t;

// 协议实例句柄
typedef struct {
    DmcState_t state;
    uint16_t seq;
    uint32_t tick;
    uint32_t timeout_ms;
    uint8_t retry_cnt;
} DmcHandle_t;

// 对外API
uint16_t dmc_crc16_ccitt(const uint8_t *buf, uint16_t len);
int dmc_send_frame(DmcHandle_t *hdl, DmcFrame_t *frame);
int dmc_recv_frame(DmcHandle_t *hdl, DmcFrame_t *frame);
int dmc_handshake_master(DmcHandle_t *hdl);
int dmc_handshake_slave(DmcHandle_t *hdl);
int dmc_send_user_data(DmcHandle_t *hdl, uint8_t *data, uint8_t data_len);

#endif
