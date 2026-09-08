/*
 * ch340_sim.h - CH340 4路编程扩展坞 · 硬件仿真模型
 *
 * 用 C 结构体精确模拟 CH340 扩展坞的硬件行为：
 *   1. 电源轨：USB 5V → 自恢复保险丝 → AMS1117-3.3 稳压 → 3.3V 总线
 *   2. 集线器：GL3520 USB 3.0 HUB，枚举 4 个下游端口
 *   3. 串口：  4× CH340C (SSOP-16, 内置晶振)，每个有完整 UART 状态机
 *   4. 烧录：  STM32 ISP / 8051 ISP 会话仿真
 *
 * 设计目标：本文档是"仿真系统"，不是真实驱动。所有状态用模型推进，
 * 可交互配置、可回放、可断言，用于教学/测试/验证扩展坞设计。
 */

#ifndef CH340_SIM_H
#define CH340_SIM_H

#include <stdint.h>
#include <stddef.h>

/* ================= 结果码 ================= */
#define CH_OK          0
#define CH_ERR_POWER  -1   /* 电源异常 */
#define CH_ERR_FUSE   -2   /* 保险丝熔断 */
#define CH_ERR_PORT   -3   /* 端口未枚举 */
#define CH_ERR_BAUD   -4   /* 非法波特率 */
#define CH_ERR_BUSY   -5   /* 通道忙 */
#define CH_ERR_PARAM  -6   /* 参数错误 */
#define CH_ERR_PROTO  -7   /* 协议错误 */

/* ================= 电源模型 ================= */
typedef struct {
    float vin;        /* USB VBUS 输入电压 (V), 典型 5.0 */
    int   fuse_ok;    /* 自恢复保险丝状态: 1=正常 0=熔断 */
    float vout;       /* AMS1117 输出电压 (V), 实际 3.30 */
    float i_load;     /* 总负载电流 (mA) */
    float i_max;      /* AMS1117 最大输出电流 (mA), 800 */
    int   overtemp;   /* 过温保护: 0=正常 1=触发 */
} ch_power_t;

/* ================= GL3520 HUB 端口 ================= */
typedef struct {
    int   present;    /* 是否有设备插入: 0=空 1=有 */
    int   enumerated; /* 枚举成功: 0=否 1=是 */
    int   speed_mbps; /* USB 速度: 480=High, 12=Full */
    int   addr;       /* USB 设备地址 (0~127) */
    char  dev_desc[64]; /* 设备描述符 (厂商/产品) */
} ch_port_t;

/* ================= CH340C 通道 ================= */
typedef enum {
    CH_TARGET_NONE = 0,   /* 未接目标 */
    CH_TARGET_STM32,      /* STM32 ISP */
    CH_TARGET_8051,       /* 8051 (STC) ISP */
    CH_TARGET_DEBUG       /* 通用调试口 */
} ch_target_t;

typedef enum {
    CH_IDLE = 0,          /* 空闲 */
    CH_TX,                /* 发送中 */
    CH_RX,                /* 接收中 */
    CH_ISP_ACTIVE,        /* ISP 会话激活 */
    CH_ISP_DONE           /* ISP 完成 */
} ch_chan_state_t;

typedef struct {
    int   online;         /* 通道在线: 0=离线 1=在线 */
    int   baud;           /* 波特率, 如 115200 */
    int   databits;       /* 数据位: 8 */
    int   stopbits;       /* 停止位: 1 */
    int   parity;         /* 校验: 0=无 */
    ch_target_t target;   /* 目标类型 */
    ch_chan_state_t state;/* 通道状态机 */
    uint8_t tx_buf[256];  /* 发送缓冲 */
    int   tx_len;         /* 发送长度 */
    int   tx_pos;         /* 发送游标 */
    uint16_t rx_byte;     /* 最近接收字节 */
    int   frame_err;      /* 帧错误计数 */
    int   bytes_sent;     /* 累计发送字节 */
    int   bytes_recv;     /* 累计接收字节 */
    /* 引脚电平 (LVTTL 3.3V) */
    int   pin_txd;        /* 1=高 0=低 */
    int   pin_rxd;
    int   pin_rts;        /* 请求发送# 有源低 */
    int   pin_dtr;        /* 数据终端就绪# 有源低 */
} ch_chan_t;

/* ================= 整个扩展坞 ================= */
typedef struct {
    ch_power_t power;         /* 电源 */
    ch_port_t  hub_ports[4];  /* GL3520 4口 */
    ch_chan_t  chans[4];      /* 4× CH340C */
    uint64_t   clock_ticks;   /* 仿真时钟 (μs) */
    int        usb_detected;  /* 主机是否识别到扩展坞 */
} ch_dock_t;

/* ================= API ================= */

/* 初始化一个扩展坞 (上电, 默认 4×CH340C 挂载) */
void ch_dock_init(ch_dock_t *dk);

/* 电源: 插上 USB 并上电 */
int ch_power_on(ch_dock_t *dk, float vin);

/* 电源: 获取当前状态到可读文本 */
const char *ch_power_status(const ch_dock_t *dk);

/* HUB: 在指定端口挂载一个 CH340C 设备 */
int ch_hub_mount(ch_dock_t *dk, int port, int speed_mbps);

/* HUB: 枚举整个 HUB, 返回成功端口数 */
int ch_hub_enumerate(ch_dock_t *dk);

/* 通道: 配置目标类型 */
int ch_chan_set_target(ch_dock_t *dk, int idx, ch_target_t t);

/* 通道: 配置波特率 */
int ch_chan_set_baud(ch_dock_t *dk, int idx, int baud);

/* 通道: 设置 DTR/RTS 控制线 (用于自动复位) */
int ch_chan_set_ctrl(ch_dock_t *dk, int idx, int rts, int dtr);

/* UART: 发送一字节 (LSB 先发, 起始位+数据+停止位) */
int ch_uart_tx_byte(ch_dock_t *dk, int idx, uint8_t byte);

/* UART: 接收一字节 (模拟从目标板收到) */
int ch_uart_rx_byte(ch_dock_t *dk, int idx, uint8_t byte);

/* 烧录: 执行一次 STM32 ISP 会话 (握手+写固件+校验) */
int ch_prog_stm32(ch_dock_t *dk, int idx, uint32_t fw_size);

/* 烧录: 执行一次 8051 ISP 会话 (冷启动+下载) */
int ch_prog_8051(ch_dock_t *dk, int idx, uint32_t code_size);

/* 状态: 打印扩展坞当前全部状态 */
void ch_dock_dump(const ch_dock_t *dk);

/* 推进仿真时钟 (模拟 UART 传输耗时), dt_micros 微秒 */
void ch_clock_advance(ch_dock_t *dk, uint64_t dt_micros);

#endif /* CH340_SIM_H */
