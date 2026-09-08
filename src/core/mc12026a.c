/*
 * mc12026a.c - MC12026A 驱动（跨平台）
 *
 * ESP8266 部分用 #ifdef 保护，macOS 编译时提供 stub。
 */

#include "mc12026a.h"
#include "ch340_sim.h"
#include <stdint.h>
#include <string.h>

/* ============================================================
 * 跨平台兼容层
 * ============================================================ */
#if defined(ARDUINO) || defined(ESP8266) || defined(ESP_PLATFORM)
  /* 真实 Arduino / ESP 环境 */
  #include <Arduino.h>
#else
  /* macOS / Linux 桌面编译 stub */
  #define ICACHE_RAM_ATTR
  #ifndef LOW
  #define LOW   0
  #define HIGH  1
  #define INPUT  0
  #define OUTPUT 1
  #define RISING  0
  #define FALLING 1
  #define CHANGE  2
  #endif
  static inline void pinMode(uint8_t, uint8_t) {}
  static inline void digitalWrite(uint8_t, uint8_t) {}
  static inline void attachInterrupt(uint8_t, void(*)(), int) {}
  static inline void detachInterrupt(uint8_t) {}
  static inline int  digitalPinToInterrupt(uint8_t p) { return p; }
  static inline void noInterrupts() {}
  static inline void interrupts() {}
  static unsigned long millis_val = 0;
  static inline unsigned long millis() { return ++millis_val; }
#endif

/* ============================================================
 * 默认 IO 回调
 * ============================================================ */

static void mc_input_io_default(void *data)
{
    (void)data;
}

MCInputIO mc_input_io = mc_input_io_default;


/* ============================================================
 * 基础函数
 * ============================================================ */

void mc_timeout(MCHandler *handler)
{
    (void)handler;
}

float mc_input(MCPoint *option)
{
    if (option == NULL) {
        return 0.0f;
    }
    return (float)option->input;
}

MCupearction *mc_open_draw(
    MCupearction *draw,
    MCupearction *input,
    MCupearction *output)
{
    if (draw == NULL || input == NULL || output == NULL) {
        return NULL;
    }
    output->input  = input->input  + draw->input;
    output->output = input->output + draw->output;
    output->out_pear    = (int16_t)(input->out_pear    + draw->out_pear);
    output->input_pear = (int16_t)(input->input_pear + draw->input_pear);
    return output;
}

MCHandler *mc_sw_draw(void *input, MCHandler *handler)
{
    if (handler == NULL) {
        return NULL;
    }
    handler->mc_handler = (input != NULL) ? 1 : 0;
    return handler;
}

MCPoint *mc_create_lowpoint(MCPoint *point)
{
    if (point == NULL) return NULL;
    point->input  = 0;
    point->output = 0;
    return point;
}

MCPoint *mc_create_heightpoint(MCPoint *point)
{
    if (point == NULL) return NULL;
    point->input  = 0;
    point->output = 0;
    return point;
}


/* ============================================================
 * 电源控制
 * ============================================================ */

int mc_power_on(ch_dock_t *dk, float vin)
{
    if (dk == NULL) return MC_ERR_PARAM;
    if (vin <= 0.0f) return MC_ERR_PARAM;
    return MC_OK;
}

MCPoint *mc_power_off(MCHandler *handler, MCPoint *point)
{
    (void)handler;
    if (point == NULL) return NULL;
    point->input  = 0;
    point->output = 0;
    return point;
}


/* ============================================================
 * ESP8266 分频器（仅嵌入式平台编译）
 * ============================================================ */
#if defined(ARDUINO) || defined(ESP8266) || defined(ESP_PLATFORM)

static volatile uint32_t g_input_pulses = 0;
static volatile uint32_t g_divider_count = 0;
static volatile uint16_t g_divider = 2;
static volatile uint8_t  g_output_state = LOW;
static uint8_t  g_input_pin = 255;
static uint8_t  g_output_pin = 255;
static volatile uint8_t  g_running = 0;
static uint8_t  g_edge = 0;
static uint32_t g_last_measure_pulses = 0;
static uint32_t g_last_measure_time = 0;
static volatile uint32_t g_frequency = 0;

ICACHE_RAM_ATTR
static void mc_divider_isr(void)
{
    if (!g_running) return;
    g_input_pulses++;
    g_divider_count++;
}

int mc_divider_init(uint8_t input_pin, uint8_t output_pin)
{
    if (input_pin > 16 || output_pin > 16) return MC_ERR_PARAM;
    if (input_pin == output_pin) return MC_ERR_PARAM;

    mc_divider_stop();
    g_input_pin  = input_pin;
    g_output_pin = output_pin;

    pinMode(g_input_pin, INPUT);
    pinMode(g_output_pin, OUTPUT);
    g_output_state = LOW;
    digitalWrite(g_output_pin, LOW);
    g_edge = 0;
    mc_divider_reset();

    attachInterrupt(
        digitalPinToInterrupt(g_input_pin),
        mc_divider_isr,
        RISING);
    return MC_OK;
}

int mc_divider_set(uint16_t divider)
{
    if (divider == 0) return MC_ERR_PARAM;
    if (divider > 32768) return MC_ERR_RANGE;

    noInterrupts();
    g_divider = divider;
    g_divider_count = 0;
    interrupts();
    return MC_OK;
}

uint16_t mc_divider_get(void)
{
    uint16_t v;
    noInterrupts();
    v = g_divider;
    interrupts();
    return v;
}

int mc_divider_set_edge(uint8_t edge)
{
    if (edge > 2) return MC_ERR_PARAM;
    if (g_input_pin > 16) return MC_ERR_STATE;

    mc_divider_stop();
    g_edge = edge;
    detachInterrupt(digitalPinToInterrupt(g_input_pin));

    int mode = (edge == 0) ? RISING : (edge == 1) ? FALLING : CHANGE;
    attachInterrupt(digitalPinToInterrupt(g_input_pin), mc_divider_isr, mode);
    return MC_OK;
}

int mc_divider_start(void)
{
    if (g_input_pin > 16 || g_output_pin > 16) return MC_ERR_STATE;
    noInterrupts();
    g_divider_count = 0;
    g_running = 1;
    interrupts();
    return MC_OK;
}

void mc_divider_stop(void)
{
    noInterrupts();
    g_running = 0;
    g_divider_count = 0;
    interrupts();
    if (g_output_pin <= 16) {
        g_output_state = LOW;
        digitalWrite(g_output_pin, LOW);
    }
}

void mc_divider_reset(void)
{
    noInterrupts();
    g_input_pulses = 0;
    g_divider_count = 0;
    interrupts();
    g_output_state = LOW;
    if (g_output_pin <= 16) digitalWrite(g_output_pin, LOW);
    g_last_measure_pulses = 0;
    g_last_measure_time = millis();
    g_frequency = 0;
}

void mc_divider_process(void)
{
    uint32_t count;
    uint16_t divider;
    uint8_t  running;

    noInterrupts();
    count   = g_divider_count;
    divider = g_divider;
    running = g_running;
    interrupts();

    if (!running) return;
    if (divider <= 1) return;

    uint32_t half_divider;
    if ((divider & 1U) == 0U) {
        half_divider = divider / 2U;
    } else {
        half_divider = (divider + 1U) / 2U;
    }

    if (count >= half_divider) {
        noInterrupts();
        if (g_divider_count >= half_divider)
            g_divider_count -= half_divider;
        else
            g_divider_count = 0;
        interrupts();

        g_output_state = !g_output_state;
        digitalWrite(g_output_pin, g_output_state);
    }

    uint32_t now = millis();
    if ((uint32_t)(now - g_last_measure_time) >= 1000U) {
        uint32_t pulses;
        noInterrupts();
        pulses = g_input_pulses;
        interrupts();
        g_frequency = pulses - g_last_measure_pulses;
        g_last_measure_pulses = pulses;
        g_last_measure_time = now;
    }
}

uint32_t mc_divider_get_pulses(void)
{
    uint32_t p;
    noInterrupts();
    p = g_input_pulses;
    interrupts();
    return p;
}

uint8_t mc_divider_get_output(void)  { return g_output_state; }
uint8_t mc_divider_running(void)     { return g_running; }

uint32_t mc_divider_get_frequency(void)
{
    uint32_t f;
    noInterrupts();
    f = g_frequency;
    interrupts();
    return f;
}

#else
/* ============================================================
 * 桌面平台 stub（分频器不编译）
 * ============================================================ */
int  mc_divider_init(uint8_t a, uint8_t b)         { (void)a; (void)b; return MC_ERR_STATE; }
int  mc_divider_set(uint16_t d)                     { (void)d; return MC_ERR_STATE; }
uint16_t mc_divider_get(void)                       { return 0; }
int  mc_divider_set_edge(uint8_t e)                 { (void)e; return MC_ERR_STATE; }
int  mc_divider_start(void)                         { return MC_ERR_STATE; }
void mc_divider_stop(void)                          {}
void mc_divider_reset(void)                         {}
void mc_divider_process(void)                       {}
uint32_t mc_divider_get_pulses(void)                { return 0; }
uint8_t  mc_divider_get_output(void)                { return 0; }
uint8_t  mc_divider_running(void)                   { return 0; }
uint32_t mc_divider_get_frequency(void)             { return 0; }
#endif
