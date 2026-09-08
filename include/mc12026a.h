#ifndef MC12026A_H
#define MC12026A_H
#include <stdint.h>
#include <stddef.h>
#include "ch340_sim.h"
#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 * ESP8266 MC12026A 软件控制接口
 * ============================================================
 *
 * 输入：
 *      GPIO 输入脉冲
 *
 * 输出：
 *      GPIO 输出分频后的方波
 *
 * 分频：
 *      divider = 1  -> Fout = Fin
 *      divider = 2  -> Fout = Fin / 2
 *      divider = 4  -> Fout = Fin / 4
 *      divider = 8  -> Fout = Fin / 8
 *
 * 推荐使用偶数分频：
 *      2 / 4 / 8 / 16 / 32 ...
 *
 * 注意：
 * ESP8266 软件 GPIO 中断不能代替真正的高速硬件分频器。
 * 如果输入频率很高，应该使用硬件计数器/外部逻辑器件。
 */


/* ============================================================
 * 返回值
 * ============================================================ */

#ifndef MC_OK
#define MC_OK               0
#endif

#ifndef MC_ERR_PARAM
#define MC_ERR_PARAM       -1
#endif

#ifndef MC_ERR_STATE
#define MC_ERR_STATE       -2
#endif

#ifndef MC_ERR_RANGE
#define MC_ERR_RANGE       -3
#endif


/* ============================================================
 * MC 值类型
 * ============================================================ */

typedef enum {
    VAL_INT = 0,
    VAL_BUFFER,
    VAL_PUT,
    VAL_VIN,
    VAL_VOUT,
    VAL_OVER,
    VAL_CLOCK,
    VAL_LOCKDER
} MCVAL;


/* ============================================================
 * 主控制结构
 * ============================================================ */

typedef struct {
    /*
     * 原来的 mc_handler 是 int16_t。
     *
     * 这里不再拿它保存指针地址。
     * ESP8266 是 32-bit，指针不能塞进 int16_t。
     *
     * mc_handler：
     *      可以作为数字句柄使用。
     */
    int16_t mc_handler;

    int32_t mc_weigth;
    int32_t mc_spi;
    int32_t mc_porint;

} MCHandler;


/* ============================================================
 * 输入输出点
 * ============================================================ */

typedef struct {

    int32_t input;
    int32_t output;

} MCPoint;


/* ============================================================
 * 高频分配
 * ============================================================ */

typedef struct {

    float input;
    float output;

    int16_t out_pear;
    int16_t input_pear;

} MCupearction;


/* ============================================================
 * 低频分配
 * ============================================================ */

typedef struct {

    float input;
    float output;

    int16_t out_pear;
    int16_t input_pear;

} MCdoeaction;


/* ============================================================
 * IO 回调
 * ============================================================ */

typedef void (*MCInputIO)(void *data);

/*
 * 真正的全局变量。
 *
 * 在 mc12026a.cpp 中定义一次。
 */
extern MCInputIO mc_input_io;


/* ============================================================
 * 基础接口
 * ============================================================ */

void mc_timeout(MCHandler *handler);

float mc_input(MCPoint *option);

MCupearction *mc_open_draw(
    MCupearction *draw,
    MCupearction *input,
    MCupearction *output
);

MCHandler *mc_sw_draw(
    void *input,
    MCHandler *handler
);

MCPoint *mc_create_lowpoint(
    MCPoint *point
);

MCPoint *mc_create_heightpoint(
    MCPoint *point
);


/* ============================================================
 * 电源控制
 * ============================================================ */

int mc_power_on(
    ch_dock_t *dk,
    float vin
);

MCPoint *mc_power_off(
    MCHandler *handler,
    MCPoint *point
);


/* ============================================================
 * ESP8266 分频器
 * ============================================================ */

/*
 * 初始化分频器
 *
 * input_pin：
 *      输入 GPIO
 *
 * output_pin：
 *      输出 GPIO
 */
int mc_divider_init(
    uint8_t input_pin,
    uint8_t output_pin
);


/*
 * 设置分频系数
 *
 * 支持：
 *
 *      1
 *      2
 *      4
 *      8
 *      16
 *      32
 *      ...
 *
 * 最大 32768。
 */
int mc_divider_set(
    uint16_t divider
);


/*
 * 获取当前分频系数
 */
uint16_t mc_divider_get(void);


/*
 * 启动分频
 */
int mc_divider_start(void);


/*
 * 停止分频
 */
void mc_divider_stop(void);


/*
 * 清零计数器
 */
void mc_divider_reset(void);


/*
 * 主循环处理函数
 *
 * 必须在 loop() 中反复调用。
 */
void mc_divider_process(void);


/*
 * 获取累计输入脉冲数
 */
uint32_t mc_divider_get_pulses(void);


/*
 * 获取输出状态
 */
uint8_t mc_divider_get_output(void);


/*
 * 获取运行状态
 */
uint8_t mc_divider_running(void);


/*
 * 获取最近一次测量的输入频率
 *
 * 单位：
 *      Hz
 */
uint32_t mc_divider_get_frequency(void);


/*
 * 设置输入 GPIO 的有效边沿
 *
 * edge：
 *
 *      0 = RISING
 *      1 = FALLING
 *      2 = CHANGE
 *
 * 推荐：
 *
 *      0
 */
int mc_divider_set_edge(
    uint8_t edge
);


#ifdef __cplusplus
}
#endif

#endif /* MC12026A_H */
