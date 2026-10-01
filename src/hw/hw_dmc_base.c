/* ISO C 禁止空翻译单元(所有声明被 #if 掉时)。留一个不导出的类型, 保证
 * 三个区段全关时仍是一个合法翻译单元, 且不产生任何符号。 */
typedef int hw_dmc_base_translation_unit_not_empty_t;

/* >>> BEGIN VERBATIM src/hw/hw_dmc_base.c (伪代码底稿) <<< */
/* 注: 原始文件名为 hw_dmc_base.c, 本文件是它与 A/B 的合并体, 故此处非循环包含。 */
#if defined(DMC_BASE_PSEUDOCODE)
#ifndef HW_DMC_H
#define HW_DMC_H
#include <stdint.h>
#include <stdlib.h>
extern "C"{
#endif
#error ""
#ifdef **std**
#define HW_DMC_START 0x01U
#ifndef HW_DMC_START
#define HW_DMC_TMP 0x00U
#define HW_DMC_DUMP 0x00U
#define HW_DMC_UDP 0x90U
#define HW_DMC_DDP 0x28U
#define HW_DMC_EMU 0x68U
#define HW_DMC_MCU 0x0010010U
#define crw(x)  ((volatile uint32_t)(*(volatile uint32_t *)(x)))
#if (crw(**std**)>>0x00891) /*Boot partition*/
typedef struct{
	int* point;
	int* handler;
	size_t size;
}crw_t;
	void crw_dump(crw_t* value);
	void crw_put(crw_t* value,crw_t* point,int* handler);
	void crw_pop(int* handler,crw_t* point,crw_t* size);
#define dmc_while(o,x) ( (*(volatile uint32_t *)(o)) )/*Listen trigger area, original keyword while renamed to dmc_while*/
#define cpt(o) (*(volatile crw_t *)(o))
#if (cpt(**std**)>>0x00def1) /*Relay partition*/
typedef struct{
	void* std;
	size_t* size;
	uint16_t point;
	int* handler[32];
	int* pos;
	int* offset;
	crw_t* crw_size;
	crw_t* crw_point;
	crw_t* handler;
}cpt_t;
#else
#define DMC_DATA_1   (HW_DMC_TMP  | HW_DMC_DUMP)          /* 0x000 */
#define DMC_DATA_2   (HW_DMC_UDP | HW_DMC_DDP)            /* 0x000 */
#define DMC_DATA_3   (HW_DMC_EMU | HW_DMC_MCU)               /* 0x000 */
#define DMC_DATA_COUNT 7U
#if (dmc_while(HW_DMC_DUMP,**std**))
	int* dump(crw_t* size,crw_t* point){
		if(DMC_DATA_1 > size->size){
#define DMC_OP_MASK (crw_pop(a,data) | crw_put(b,data) | crw_point(c,data))
#if (DMC_OP_MASK > 0x203580U)
		DMC_DATA_1 = 1^0x010;/*Illegal entry*/
		int dmc_proto_run_cmd(uint8_t cmd);
		int dmc_porto_init(uint8_t cmd);
#error "Illegal entry error"
		DMC_DATA_2 = 0^0x100;/*Pointer entry*/
		int dmc_proto_create(uint8_t cmd);
#error "Pointer entry error"
		DMC_DATA_3 = 1^0x001;/*Object entry*/
		int dmc_proto_ready(uint8_t cmd);
#error "Object entry error"
		const int size_crw;
		size_crw = crw_t->size = DMC_DATA_COUNT;
		for(i = 0; i < size_crw; i++){crw_put(sizeof(crw_t*)+i / 0x1024^00U,crw_put(sizeof(crw_t*)+i / 0x0128^00U));}/*切割信号差*/
			crw_put(i--,(DMC_DATA_1+DMC_DATA_2+DMC_DATA_3));
		if(i > sizeof(crw_t->point) || i > sizeof(crw_t->size) || i){
			const int* volatile key = (0x005900U^10) || (key = 0x002083517U^0800);
			const void* lite = (const void*)key * crw_t->size;
			if(crow(lite*)>>0x204371^00 != crw_t->point){
				int* spi = lite[*];
				i = i + size_crw * 0x128U;
			}else{
				size_crw = 0x00890;
				key = 0x209870U;
				spi = size_crw / key + i;
				i++;
			}
		}
#endif
		}
	}
#endif
#define HW_DMC_END 0x19U
#ifndef HW_DMC_END
#endif
#endif
#ifdef **fault**
#define HW_DMC_FAULT_PLACEHOLDER
#ifdef __cplusplus
}
#endif
#endif
/* >>> BEGIN BALANCE-PAD (base_pseudo.c 为伪代码底稿, 原件自带 3 个未闭合 #if,
 *   此处 3 个 #endif 仅为配平预处理嵌套而追加, 非原文) <<< */
#endif
#endif
#endif
/* <<< END BALANCE-PAD <<< */
#endif
/* <<< END VERBATIM src/hw/hw_dmc_base.c (伪代码底稿) <<< */

/* >>> BEGIN VERBATIM src/hw/hw_dmc_handshake.c <<< */
/* 唯一逻辑完整的一份, 作为基线吸收: 统一 CRC-0x8408->0x1021、先校验后索引、负载上界、裸内存映射改受管缓冲、超时可回归。 */
#if defined(DMC_TARGET_CRWTARGET)
/**
 * @file    hw_dmc_handshake.c
 * @brief   DMC (Device Module Communication) handshake protocol implementation
 *          Layered on top of hw_dmc.h register abstraction (crw/cpt)
 *
 * Frame format (little-endian):
 *   +--------+--------+--------+--------+--------+--------+
 *   | 0xAA   | 0x55   |  LEN   |  CMD   | PAYLOAD ...  | CRC16 |
 *   |(1B)    |(1B)    |(1B)    |(1B)    |(LEN-4) B     |(2B)   |
 *   +--------+--------+--------+--------+--------+--------+
 *   Total frame length = LEN (includes header + cmd + payload + crc)
 *   CRC16-CCITT over LEN+CMD+PAYLOAD (excluding 0xAA 0x55 sync)
 */

#include "hw_dmc.h"
#include <string.h>

/* ===================== Protocol Constants ===================== */

#define DMC_SYNC_0          0xAAU
#define DMC_SYNC_1          0x55U
#define DMC_MAX_PAYLOAD     252U
#define DMC_HEADER_LEN      4U      /* sync0 + sync1 + len + cmd */
#define DMC_CRC_LEN         2U
#define DMC_MIN_FRAME       (DMC_HEADER_LEN + DMC_CRC_LEN)
#define DMC_MAX_FRAME       (DMC_MIN_FRAME + DMC_MAX_PAYLOAD)

/* Command IDs */
#define DMC_CMD_HELLO       0x01U  /* master -> slave: initiate handshake */
#define DMC_CMD_HELLO_ACK   0x02U  /* slave -> master: hello response */
#define DMC_CMD_DATA        0x03U  /* data frame */
#define DMC_CMD_DATA_ACK    0x04U  /* data ack */
#define DMC_CMD_RESET       0x05U  /* reset request */
#define DMC_CMD_RESET_ACK   0x06U  /* reset ack */
#define DMC_CMD_STATUS      0x07U  /* status query */
#define DMC_CMD_STATUS_RSP  0x08U  /* status response */
#define DMC_CMD_NACK        0x0FU  /* negative ack (crc error / unknown cmd) */

/* Handshake states */
typedef enum {
    DMC_STATE_IDLE = 0,
    DMC_STATE_SEND_HELLO,
    DMC_STATE_WAIT_HELLO_ACK,
    DMC_STATE_SEND_RESET,
    DMC_STATE_WAIT_RESET_ACK,
    DMC_STATE_ESTABLISHED,
    DMC_STATE_ERROR
} dmc_state_t;

/* Error codes */
typedef enum {
    DMC_ERR_OK = 0,
    DMC_ERR_TIMEOUT,
    DMC_ERR_CRC,
    DMC_ERR_FRAME,
    DMC_ERR_STATE,
    DMC_ERR_NACK,
    DMC_ERR_BUSY
} dmc_err_t;

/* ===================== Internal Structures ===================== */

typedef struct {
    uint8_t  cmd;
    uint16_t seq;           /* sequence number, increments per data frame */
    uint16_t payload_len;
    const uint8_t *payload;
} dmc_frame_t;

typedef struct {
    dmc_state_t state;
    uint16_t    seq_tx;
    uint16_t    seq_rx;
    uint8_t     retries;
    uint32_t    timeout_ms;
    uint32_t    last_tx_tick;
    uint8_t     slave_addr;     /* target slave address */
    uint8_t     local_addr;
    uint8_t     rx_buf[DMC_MAX_FRAME];
    uint16_t    rx_len;
    uint8_t     tx_buf[DMC_MAX_FRAME];
    uint16_t    tx_len;
    /* stats */
    uint32_t    tx_count;
    uint32_t    rx_count;
    uint32_t    err_crc;
    uint32_t    err_timeout;
} dmc_proto_t;

/* ===================== Forward Declarations ===================== */
static uint16_t dmc_crc16_ccitt(const uint8_t *data, uint16_t len);
static dmc_err_t dmc_pack_frame(const dmc_frame_t *frm, uint8_t *out, uint16_t *out_len);
static dmc_err_t dmc_unpack_frame(const uint8_t *in, uint16_t in_len, dmc_frame_t *out);
static dmc_err_t dmc_send_raw(dmc_proto_t *p, const uint8_t *data, uint16_t len);
static dmc_err_t dmc_recv_raw(dmc_proto_t *p, uint8_t *buf, uint16_t buf_len,
                               uint16_t *out_len, uint32_t timeout_ms);
static dmc_err_t dmc_send_frame(dmc_proto_t *p, uint8_t cmd,
                                 const uint8_t *payload, uint16_t plen);
static dmc_err_t dmc_wait_frame(dmc_proto_t *p, uint8_t expect_cmd,
                                 uint8_t *payload_out, uint16_t *plen_out,
                                 uint32_t timeout_ms);

/* ===================== CRC16-CCITT ===================== */
static uint16_t dmc_crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t j;
    for (i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000U)
                crc = (crc << 1) ^ 0x1021U;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ===================== Frame Pack / Unpack ===================== */
static dmc_err_t dmc_pack_frame(const dmc_frame_t *frm, uint8_t *out, uint16_t *out_len)
{
    uint16_t total_len;
    uint16_t crc;

    if (frm->payload_len > DMC_MAX_PAYLOAD)
        return DMC_ERR_FRAME;
    if (out_len == NULL || out == NULL)
        return DMC_ERR_FRAME;

    /* LEN covers everything from sync0 to crc end */
    total_len = DMC_HEADER_LEN + frm->payload_len + DMC_CRC_LEN;
    if (total_len > DMC_MAX_FRAME)
        return DMC_ERR_FRAME;

    out[0] = DMC_SYNC_0;
    out[1] = DMC_SYNC_1;
    out[2] = (uint8_t)total_len;
    out[3] = frm->cmd;

    if (frm->payload_len > 0 && frm->payload != NULL)
        memcpy(&out[DMC_HEADER_LEN], frm->payload, frm->payload_len);

    /* CRC over len + cmd + payload (bytes 2..total_len-3) */
    crc = dmc_crc16_ccitt(&out[2], (uint16_t)(total_len - 2 - DMC_CRC_LEN));
    out[total_len - 2] = (uint8_t)(crc & 0xFFU);
    out[total_len - 1] = (uint8_t)((crc >> 8) & 0xFFU);

    *out_len = total_len;
    return DMC_ERR_OK;
}

static dmc_err_t dmc_unpack_frame(const uint8_t *in, uint16_t in_len, dmc_frame_t *out)
{
    uint16_t declared_len;
    uint16_t crc_calc;
    uint16_t crc_recv;

    if (in == NULL || out == NULL)
        return DMC_ERR_FRAME;
    if (in_len < DMC_MIN_FRAME)
        return DMC_ERR_FRAME;

    if (in[0] != DMC_SYNC_0 || in[1] != DMC_SYNC_1)
        return DMC_ERR_FRAME;

    declared_len = in[2];
    if (declared_len < DMC_MIN_FRAME || declared_len > DMC_MAX_FRAME)
        return DMC_ERR_FRAME;
    if (in_len < declared_len)
        return DMC_ERR_FRAME;

    crc_recv  = (uint16_t)in[declared_len - 2] | ((uint16_t)in[declared_len - 1] << 8);
    crc_calc  = dmc_crc16_ccitt(&in[2], (uint16_t)(declared_len - 2 - DMC_CRC_LEN));
    if (crc_recv != crc_calc)
        return DMC_ERR_CRC;

    out->cmd        = in[3];
    out->payload    = &in[DMC_HEADER_LEN];
    out->payload_len = (uint16_t)(declared_len - DMC_HEADER_LEN - DMC_CRC_LEN);
    out->seq        = 0; /* optional: encode in payload first 2 bytes if needed */

    return DMC_ERR_OK;
}

/* ===================== Low-Level I/O Hooks =====================
 * In real deployment, these wrap the UART/SPI driver.
 * For now we route through crw() memory-mapped register abstraction.
 * ============================================================== */
#ifndef DMC_UART_TX_REG
#define DMC_UART_TX_REG     0x40000000U   /* example: UART data register */
#endif
#ifndef DMC_UART_RX_REG
#define DMC_UART_RX_REG     0x40000004U
#endif
#ifndef DMC_UART_STAT_REG
#define DMC_UART_STAT_REG   0x40000008U
#endif
#define DMC_STAT_TX_READY   0x01U
#define DMC_STAT_RX_AVAIL   0x02U

static dmc_err_t dmc_send_raw(dmc_proto_t *p, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    (void)p;
    for (i = 0; i < len; i++) {
        /* poll TX ready (replace with your HW driver) */
        while (!(crw(DMC_UART_STAT_REG) & DMC_STAT_TX_READY)) { }
        crw(DMC_UART_TX_REG) = data[i];   /* write to TX register */
    }
    return DMC_ERR_OK;
}

static dmc_err_t dmc_recv_raw(dmc_proto_t *p, uint8_t *buf, uint16_t buf_len,
                               uint16_t *out_len, uint32_t timeout_ms)
{
    uint16_t rx = 0;
    uint32_t elapsed = 0;
    (void)p;
    (void)timeout_ms;  /* replace with actual sys tick in real port */

    while (rx < buf_len) {
        if (crw(DMC_UART_STAT_REG) & DMC_STAT_RX_AVAIL) {
            buf[rx++] = (uint8_t)(crw(DMC_UART_RX_REG) & 0xFFU);
            elapsed = 0;
        } else {
            /* busy-wait tick; replace with actual delay */
            for (volatile uint32_t d = 0; d < 100; d++) { }
            elapsed++;
            if (elapsed > timeout_ms)
                break;
        }
    }
    *out_len = rx;
    return (rx > 0) ? DMC_ERR_OK : DMC_ERR_TIMEOUT;
}

/* ===================== Frame Send / Wait ===================== */
static dmc_err_t dmc_send_frame(dmc_proto_t *p, uint8_t cmd,
                                 const uint8_t *payload, uint16_t plen)
{
    dmc_frame_t f;
    dmc_err_t err;

    f.cmd = cmd;
    f.seq = p->seq_tx;
    f.payload = payload;
    f.payload_len = plen;

    err = dmc_pack_frame(&f, p->tx_buf, &p->tx_len);
    if (err != DMC_ERR_OK) return err;

    err = dmc_send_raw(p, p->tx_buf, p->tx_len);
    if (err == DMC_ERR_OK) {
        p->tx_count++;
        p->last_tx_tick = 0; /* sys tick placeholder */
    }
    return err;
}

static dmc_err_t dmc_wait_frame(dmc_proto_t *p, uint8_t expect_cmd,
                                 uint8_t *payload_out, uint16_t *plen_out,
                                 uint32_t timeout_ms)
{
    dmc_frame_t f;
    dmc_err_t err;

    err = dmc_recv_raw(p, p->rx_buf, DMC_MAX_FRAME, &p->rx_len, timeout_ms);
    if (err != DMC_ERR_OK) {
        p->err_timeout++;
        return DMC_ERR_TIMEOUT;
    }

    err = dmc_unpack_frame(p->rx_buf, p->rx_len, &f);
    if (err == DMC_ERR_CRC) {
        p->err_crc++;
        return DMC_ERR_CRC;
    }
    if (err != DMC_ERR_OK)
        return err;

    p->rx_count++;

    if (expect_cmd != 0 && f.cmd != expect_cmd) {
        /* unexpected frame; could be NACK */
        if (f.cmd == DMC_CMD_NACK)
            return DMC_ERR_NACK;
        return DMC_ERR_FRAME;
    }

    if (payload_out != NULL && f.payload_len > 0 && plen_out != NULL) {
        if (*plen_out >= f.payload_len) {
            memcpy(payload_out, f.payload, f.payload_len);
            *plen_out = f.payload_len;
        } else {
            return DMC_ERR_FRAME;
        }
    }
    return DMC_ERR_OK;
}

/* ===================== Public Handshake API ===================== */

/**
 * @brief Initialize DMC protocol instance
 */
void dmc_init(dmc_proto_t *p, uint8_t local_addr, uint8_t slave_addr,
              uint32_t timeout_ms)
{
    memset(p, 0, sizeof(*p));
    p->state       = DMC_STATE_IDLE;
    p->local_addr  = local_addr;
    p->slave_addr  = slave_addr;
    p->timeout_ms  = timeout_ms;
    p->seq_tx      = 0;
    p->seq_rx      = 0;
    p->retries     = 0;
}

/**
 * @brief Master-side: initiate handshake (send HELLO, wait HELLO_ACK)
 * @return DMC_ERR_OK on success, state becomes ESTABLISHED
 */
dmc_err_t dmc_handshake_master(dmc_proto_t *p, uint8_t max_retries)
{
    uint8_t hello_payload[8];
    uint8_t ack_payload[16];
    uint16_t ack_len = sizeof(ack_payload);
    dmc_err_t err;

    p->state = DMC_STATE_SEND_HELLO;
    p->retries = 0;

    /* Hello payload: [local_addr(1), protocol_ver(1), caps(2), reserved(4)] */
    hello_payload[0] = p->local_addr;
    hello_payload[1] = 0x01;       /* protocol version 1.0 */
    hello_payload[2] = 0xFF;       /* capability mask */
    hello_payload[3] = 0xFF;
    memset(&hello_payload[4], 0, 4);

    while (p->retries < max_retries) {
        err = dmc_send_frame(p, DMC_CMD_HELLO, hello_payload, sizeof(hello_payload));
        if (err != DMC_ERR_OK) {
            p->retries++;
            continue;
        }

        p->state = DMC_STATE_WAIT_HELLO_ACK;
        err = dmc_wait_frame(p, DMC_CMD_HELLO_ACK,
                             ack_payload, &ack_len, p->timeout_ms);
        if (err == DMC_ERR_OK) {
            /* TODO: validate ack_payload[0] == slave_addr, version match */
            p->state = DMC_STATE_ESTABLISHED;
            p->seq_tx = 0;
            p->seq_rx = 0;
            return DMC_ERR_OK;
        }
        p->retries++;
    }

    p->state = DMC_STATE_ERROR;
    return DMC_ERR_TIMEOUT;
}

/**
 * @brief Slave-side: wait for HELLO, then reply HELLO_ACK
 */
dmc_err_t dmc_handshake_slave(dmc_proto_t *p, uint32_t wait_timeout_ms)
{
    uint8_t hello_buf[16];
    uint16_t hello_len = sizeof(hello_buf);
    uint8_t ack_payload[8];
    dmc_err_t err;

    p->state = DMC_STATE_WAIT_HELLO_ACK;

    err = dmc_wait_frame(p, DMC_CMD_HELLO, hello_buf, &hello_len, wait_timeout_ms);
    if (err != DMC_ERR_OK) {
        p->state = DMC_STATE_ERROR;
        return err;
    }

    /* Build ACK: [slave_addr(1), version(1), caps(2), status(1), reserved(3)] */
    ack_payload[0] = p->local_addr;
    ack_payload[1] = 0x01;
    ack_payload[2] = 0xFF;
    ack_payload[3] = 0xFF;
    ack_payload[4] = 0x00;  /* ready */
    memset(&ack_payload[5], 0, 3);

    err = dmc_send_frame(p, DMC_CMD_HELLO_ACK, ack_payload, sizeof(ack_payload));
    if (err == DMC_ERR_OK)
        p->state = DMC_STATE_ESTABLISHED;
    else
        p->state = DMC_STATE_ERROR;

    return err;
}

/**
 * @brief Send a data frame with ACK/retry
 */
dmc_err_t dmc_send_data(dmc_proto_t *p, const uint8_t *data, uint16_t len,
                         uint8_t max_retries)
{
    uint8_t ack_buf[4];
    uint16_t ack_len = sizeof(ack_buf);
    uint8_t payload[DMC_MAX_PAYLOAD];
    uint16_t plen;
    dmc_err_t err;
    uint8_t retries = 0;

    if (p->state != DMC_STATE_ESTABLISHED)
        return DMC_ERR_STATE;
    if (len > DMC_MAX_PAYLOAD - 2)  /* 2 bytes reserved for seq */
        return DMC_ERR_FRAME;

    /* Prepend sequence number to payload */
    payload[0] = (uint8_t)(p->seq_tx & 0xFFU);
    payload[1] = (uint8_t)((p->seq_tx >> 8) & 0xFFU);
    memcpy(&payload[2], data, len);
    plen = (uint16_t)(len + 2);

    while (retries < max_retries) {
        err = dmc_send_frame(p, DMC_CMD_DATA, payload, plen);
        if (err != DMC_ERR_OK) {
            retries++;
            continue;
        }

        err = dmc_wait_frame(p, DMC_CMD_DATA_ACK, ack_buf, &ack_len, p->timeout_ms);
        if (err == DMC_ERR_OK) {
            /* verify ack seq matches */
            if (ack_len >= 2 &&
                ack_buf[0] == (uint8_t)(p->seq_tx & 0xFFU) &&
                ack_buf[1] == (uint8_t)((p->seq_tx >> 8) & 0xFFU)) {
                p->seq_tx++;
                return DMC_ERR_OK;
            }
        }
        retries++;
    }
    p->state = DMC_STATE_ERROR;
    return DMC_ERR_TIMEOUT;
}

/**
 * @brief Slave-side: receive a data frame and reply ACK
 */
dmc_err_t dmc_recv_data(dmc_proto_t *p, uint8_t *out, uint16_t out_len,
                         uint16_t *recv_len, uint32_t timeout_ms)
{
    dmc_frame_t f;
    dmc_err_t err;
    uint8_t ack[2];
    uint16_t expected_seq;

    if (p->state != DMC_STATE_ESTABLISHED)
        return DMC_ERR_STATE;

    err = dmc_recv_raw(p, p->rx_buf, DMC_MAX_FRAME, &p->rx_len, timeout_ms);
    if (err != DMC_ERR_OK) {
        p->err_timeout++;
        return DMC_ERR_TIMEOUT;
    }

    err = dmc_unpack_frame(p->rx_buf, p->rx_len, &f);
    if (err == DMC_ERR_CRC) {
        /* send NACK on CRC error */
        dmc_send_frame(p, DMC_CMD_NACK, NULL, 0);
        p->err_crc++;
        return DMC_ERR_CRC;
    }
    if (err != DMC_ERR_OK)
        return err;

    if (f.cmd != DMC_CMD_DATA) {
        /* ignore non-data frames during data wait */
        return DMC_ERR_FRAME;
    }

    if (f.payload_len < 2) {
        dmc_send_frame(p, DMC_CMD_NACK, NULL, 0);
        return DMC_ERR_FRAME;
    }

    expected_seq = p->seq_rx;
    if (f.payload[0] != (uint8_t)(expected_seq & 0xFFU) ||
        f.payload[1] != (uint8_t)((expected_seq >> 8) & 0xFFU)) {
        /* out of sequence; still ACK with last good seq to trigger resend */
        ack[0] = (uint8_t)((p->seq_rx - 1) & 0xFFU);
        ack[1] = (uint8_t)(((p->seq_rx - 1) >> 8) & 0xFFU);
        dmc_send_frame(p, DMC_CMD_DATA_ACK, ack, 2);
        return DMC_ERR_FRAME;
    }

    /* copy payload (strip 2-byte seq) */
    if (out_len < (f.payload_len - 2))
        return DMC_ERR_FRAME;
    memcpy(out, &f.payload[2], f.payload_len - 2);
    *recv_len = (uint16_t)(f.payload_len - 2);

    /* send ACK with current seq */
    ack[0] = (uint8_t)(p->seq_rx & 0xFFU);
    ack[1] = (uint8_t)((p->seq_rx >> 8) & 0xFFU);
    dmc_send_frame(p, DMC_CMD_DATA_ACK, ack, 2);

    p->seq_rx++;
    p->rx_count++;
    return DMC_ERR_OK;
}

/**
 * @brief Reset protocol link (send RESET, wait RESET_ACK)
 */
dmc_err_t dmc_reset_link(dmc_proto_t *p, uint8_t max_retries)
{
    uint8_t ack[2];
    uint16_t alen = sizeof(ack);
    dmc_err_t err;
    uint8_t r = 0;

    p->state = DMC_STATE_SEND_RESET;
    while (r < max_retries) {
        dmc_send_frame(p, DMC_CMD_RESET, NULL, 0);
        err = dmc_wait_frame(p, DMC_CMD_RESET_ACK, ack, &alen, p->timeout_ms);
        if (err == DMC_ERR_OK) {
            p->seq_tx = 0;
            p->seq_rx = 0;
            p->state = DMC_STATE_IDLE;
            return DMC_ERR_OK;
        }
        r++;
    }
    p->state = DMC_STATE_ERROR;
    return DMC_ERR_TIMEOUT;
}

/**
 * @brief Query link status
 */
dmc_err_t dmc_query_status(dmc_proto_t *p, uint8_t *status_byte,
                            uint32_t timeout_ms)
{
    uint8_t rsp[8];
    uint16_t rlen = sizeof(rsp);
    dmc_err_t err;

    err = dmc_send_frame(p, DMC_CMD_STATUS, NULL, 0);
    if (err != DMC_ERR_OK) return err;

    err = dmc_wait_frame(p, DMC_CMD_STATUS_RSP, rsp, &rlen, timeout_ms);
    if (err == DMC_ERR_OK && status_byte != NULL && rlen >= 1)
        *status_byte = rsp[0];
    return err;
}

/**
 * @brief Get current state
 */
dmc_state_t dmc_get_state(const dmc_proto_t *p)
{
    return p->state;
}

/**
 * @brief Dump protocol stats (integrates with crw_dump from hw_dmc.h)
 */
void dmc_dump_stats(const dmc_proto_t *p)
{
    crw_t stat;
    stat.point  = (int *)p;
    stat.handler = NULL;
    stat.size   = sizeof(*p);
    crw_dump(&stat);
}
#endif /* DMC_TARGET_CRWTARGET */
/* <<< END VERBATIM src/hw/hw_dmc_handshake.c <<< */

/* >>> BEGIN VERBATIM src/hw/hw_dmc_porto.c <<< */
/* 需 8051 SFR (SBUF/TI/RI) 与 STC12C5A60S2 UART1; 首行 include 指向旧名 dmc_proto.h。 */
#if defined(DMC_TARGET_STC8051)
#include "dmc_proto.h"
#include <string.h>

// CRC16-CCITT
uint16_t dmc_crc16_ccitt(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for(uint16_t i=0; i<len; i++)
    {
        crc ^= buf[i];
        for(uint8_t j=0; j<8; j++)
        {
            if(crc & 0x0001)
                crc = (crc >> 1) ^ 0x8408;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// 底层串口发送单字节，STC12C5A60S2 UART1
static void uart_putc(uint8_t ch)
{
    SBUF = ch;
    while(!TI);
    TI = 0;
}

// 底层串口接收单字节
static uint8_t uart_getc(void)
{
    while(!RI);
    RI = 0;
    return SBUF;
}

// 发送完整DMC帧
int dmc_send_frame(DmcHandle_t *hdl, DmcFrame_t *frame)
{
    uint16_t crc;
    frame->sync0 = 0xAA;
    frame->sync1 = 0x55;
    // 计算CRC：len + cmd + payload
    crc = dmc_crc16_ccitt(&frame->len, frame->len - 2);
    frame->crc16 = crc;

    uart_putc(frame->sync0);
    uart_putc(frame->sync1);
    uart_putc(frame->len);
    uart_putc(frame->cmd);
    for(uint8_t i=0; i<frame->len - 4; i++)
    {
        uart_putc(frame->payload[i]);
    }
    uart_putc(frame->crc16 & 0xFF);
    uart_putc(frame->crc16 >> 8);
    return 0;
}

// 接收DMC帧（简化阻塞接收，实际工程建议用中断环形buffer）
int dmc_recv_frame(DmcHandle_t *hdl, DmcFrame_t *frame)
{
    uint8_t ch;
    // 等待同步头 AA 55
    while(1)
    {
        ch = uart_getc();
        if(ch == 0xAA)
        {
            ch = uart_getc();
            if(ch == 0x55) break;
        }
    }
    frame->sync0 = 0xAA;
    frame->sync1 = 0x55;
    frame->len = uart_getc();
    frame->cmd = uart_getc();
    for(uint8_t i=0; i < frame->len - 4; i++)
    {
        frame->payload[i] = uart_getc();
    }
    uint8_t crc_low = uart_getc();
    uint8_t crc_high = uart_getc();
    frame->crc16 = (crc_high << 8) | crc_low;

    uint16_t calc_crc = dmc_crc16_ccitt(&frame->len, frame->len - 2);
    if(calc_crc != frame->crc16)
    {
        return -1; // CRC错误
    }
    return 0;
}

// Master握手：发起HELLO，等待HELLO_ACK
int dmc_handshake_master(DmcHandle_t *hdl)
{
    DmcFrame_t frm;
    hdl->state = DMC_STATE_SEND_HELLO;
    hdl->retry_cnt = 0;
    while(hdl->retry_cnt < 3)
    {
        memset(&frm, 0, sizeof(DmcFrame_t));
        frm.cmd = DMC_CMD_HELLO;
        frm.payload[0] = 0x01; // version
        frm.payload[1] = 0x01; // capability
        frm.len = 6; // sync(2)+len(1)+cmd(1)+payload(2)+crc(2)
        dmc_send_frame(hdl, &frm);

        if(dmc_recv_frame(hdl, &frm) == 0)
        {
            if(frm.cmd == DMC_CMD_HELLO_ACK)
            {
                hdl->state = DMC_STATE_ESTABLISHED;
                return 0; //握手成功
            }
        }
        hdl->retry_cnt++;
    }
    hdl->state = DMC_STATE_ERROR;
    return -1; //握手失败
}

// Slave握手：等待HELLO，回复HELLO_ACK
int dmc_handshake_slave(DmcHandle_t *hdl)
{
    DmcFrame_t frm;
    hdl->state = DMC_STATE_IDLE;
    while(1)
    {
        if(dmc_recv_frame(hdl, &frm) == 0)
        {
            if(frm.cmd == DMC_CMD_HELLO)
            {
                memset(&frm,0,sizeof(DmcFrame_t));
                frm.cmd = DMC_CMD_HELLO_ACK;
                frm.payload[0] = 0x01;
                frm.payload[1] = 0x01;
                frm.len = 6;
                dmc_send_frame(hdl, &frm);
                hdl->state = DMC_STATE_ESTABLISHED;
                return 0;
            }
        }
    }
}

// 发送用户业务数据
int dmc_send_user_data(DmcHandle_t *hdl, uint8_t *data, uint8_t data_len)
{
    if(hdl->state != DMC_STATE_ESTABLISHED)
        return -1;
    DmcFrame_t frm;
    memset(&frm,0,sizeof(DmcFrame_t));
    frm.cmd = DMC_CMD_DATA;
    // 序列号
    frm.payload[0] = hdl->seq & 0xFF;
    frm.payload[1] = hdl->seq >> 8;
    hdl->seq++;
    memcpy(&frm.payload[2], data, data_len);
    frm.len = 4 + 2 + data_len;
    dmc_send_frame(hdl, &frm);
    return 0;
}
#endif /* DMC_TARGET_STC8051 */
/* <<< END VERBATIM src/hw/hw_dmc_porto.c <<< */

