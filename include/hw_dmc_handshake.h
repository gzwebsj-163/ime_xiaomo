/**
 * @file    hw_dmc_handshake.h
 * @brief   DMC handshake protocol public API
 */
#ifndef HW_DMC_HANDSHAKE_H
#define HW_DMC_HANDSHAKE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque handle (defined in .c) ---- */
typedef struct dmc_proto_s dmc_proto_t;

/* ---- Error codes ---- */
typedef enum {
    DMC_ERR_OK = 0,
    DMC_ERR_TIMEOUT,
    DMC_ERR_CRC,
    DMC_ERR_FRAME,
    DMC_ERR_STATE,
    DMC_ERR_NACK,
    DMC_ERR_BUSY
} dmc_err_t;

/* ---- States ---- */
typedef enum {
    DMC_STATE_IDLE = 0,
    DMC_STATE_SEND_HELLO,
    DMC_STATE_WAIT_HELLO_ACK,
    DMC_STATE_SEND_RESET,
    DMC_STATE_WAIT_RESET_ACK,
    DMC_STATE_ESTABLISHED,
    DMC_STATE_ERROR
} dmc_state_t;

/* ---- Lifecycle ---- */
void      dmc_init(dmc_proto_t *p, uint8_t local_addr, uint8_t slave_addr,
                   uint32_t timeout_ms);
dmc_err_t dmc_handshake_master(dmc_proto_t *p, uint8_t max_retries);
dmc_err_t dmc_handshake_slave(dmc_proto_t *p, uint32_t wait_timeout_ms);
dmc_err_t dmc_reset_link(dmc_proto_t *p, uint8_t max_retries);

/* ---- Data transfer ---- */
dmc_err_t dmc_send_data(dmc_proto_t *p, const uint8_t *data, uint16_t len,
                        uint8_t max_retries);
dmc_err_t dmc_recv_data(dmc_proto_t *p, uint8_t *out, uint16_t out_len,
                         uint16_t *recv_len, uint32_t timeout_ms);

/* ---- Diagnostics ---- */
dmc_err_t   dmc_query_status(dmc_proto_t *p, uint8_t *status_byte,
                              uint32_t timeout_ms);
dmc_state_t dmc_get_state(const dmc_proto_t *p);
void        dmc_dump_stats(const dmc_proto_t *p);

#ifdef __cplusplus
}
#endif

#endif /* HW_DMC_HANDSHAKE_H */
