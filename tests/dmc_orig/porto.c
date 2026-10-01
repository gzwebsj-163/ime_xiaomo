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
