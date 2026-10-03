/**
 * i18n.c / i18n.h — 多语言文案表（自动生成，⚠️ 勿手改）
 *
 * 由 tools/gen_i18n.py 从 main/i18n/strings.txt 生成。
 * 加文案的正确做法：改 strings.txt → 重跑 gen_i18n.py → 重跑 gen_font.py
 *                   （字体子集也依赖文案表！漏了 gen_font.py 会出现方块字）
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 语言索引 */
typedef enum {
    UI_LANG_ZH = 0,   /* 中文（出厂默认） */
    UI_LANG_EN = 1,   /* English */
    UI_LANG_COUNT = 2
} ui_lang_t;

/** 语言范围（写入存储用；UI_LANG_FIRST 恒为出厂默认中文） */
typedef enum {
    UI_LANG_FIRST = 0,       /* = UI_LANG_ZH，出厂默认 */
    UI_LANG_LAST  = 1,      /* = UI_LANG_EN */
} ui_lang_range_t;

/** 运行时当前语言（UI 每次取文案都读它） */
extern uint8_t g_ui_lang;

/** 语言显示名（语言选择页用：中文 / English） */
extern const char *const UI_LANG_NAME[2];

/** 缺失 key 的占位串（醒目，非空） */
extern const char *const UI_STR_MISSING;

/** 文案总数（自检用） */
#define UI_STR_COUNT 191

/* ---- key → 索引（编译期常量；ui.c 用它调 ui_tr） ---- */
#define TID_S_OK 0u
#define TID_S_CANCEL 1u
#define TID_S_BACK 2u
#define TID_S_SAVE 3u
#define TID_S_SAVED 4u
#define TID_S_YES 5u
#define TID_S_NO 6u
#define TID_S_ON 7u
#define TID_S_OFF 8u
#define TID_S_NONE 9u
#define TID_S_AUTO 10u
#define TID_S_MANUAL 11u
#define TID_S_DEFAULT 12u
#define TID_S_ERROR 13u
#define TID_S_FAIL 14u
#define TID_S_PASS 15u
#define TID_S_READY 16u
#define TID_S_BUSY 17u
#define TID_S_WAIT 18u
#define TID_S_UNKNOWN 19u
#define TID_S_NONE_DETECTED 20u
#define TID_S_ARE_YOU_SURE 21u
#define TID_S_CONFIRM 22u
#define TID_S_LOADING 23u
#define TID_S_VERSION 24u
#define TID_S_TOTAL 25u
#define TID_W_BRAND 26u
#define TID_W_BOOTING 27u
#define TID_W_SKIP 28u
#define TID_W_SETUP_FIRST 29u
#define TID_W_SETUP_AGAIN 30u
#define TID_W_STEP_LANG 31u
#define TID_W_STEP_TIME 32u
#define TID_W_STEP_DONE 33u
#define TID_W_LANG_TITLE 34u
#define TID_W_LANG_ZH 35u
#define TID_W_LANG_EN 36u
#define TID_W_TIME_TITLE 37u
#define TID_W_TIME_YEAR 38u
#define TID_W_TIME_MONTH 39u
#define TID_W_TIME_DAY 40u
#define TID_W_TIME_HOUR 41u
#define TID_W_TIME_MIN 42u
#define TID_W_TIME_SEC 43u
#define TID_W_TIME_UP 44u
#define TID_W_TIME_DOWN 45u
#define TID_W_TIME_NEXT 46u
#define TID_W_TIME_SAVED 47u
#define TID_W_SETUP_DONE 48u
#define TID_W_ENTER_MENU 49u
#define TID_M_PROBE 50u
#define TID_M_FLASH 51u
#define TID_M_DMC 52u
#define TID_M_DC 53u
#define TID_M_DIRECT 54u
#define TID_M_FAULT 55u
#define TID_M_SETUP 56u
#define TID_M_STATUS 57u
#define TID_M_ABOUT 58u
#define TID_P_PROBE_TITLE 59u
#define TID_P_PROBE_PROFILE 60u
#define TID_P_PROBE_DRIVER 61u
#define TID_P_PROBE_JEDEC 62u
#define TID_P_PROBE_START 63u
#define TID_P_PROBE_VENDOR 64u
#define TID_P_PROBE_DEV 65u
#define TID_P_PROBE_CAPACITY 66u
#define TID_P_PROBE_SERIAL 67u
#define TID_P_PROBE_FOUND 68u
#define TID_P_PROBE_NO_CHIP 69u
#define TID_P_PROBE_TIP_CHECK 70u
#define TID_P_PROBE_TIP_PROBE 71u
#define TID_P_PROBE_SIM 72u
#define TID_P_PROG_TARGET 73u
#define TID_P_PROG_DRIVER 74u
#define TID_P_PROG_IMAGE 75u
#define TID_P_PROG_READ_ID 76u
#define TID_P_PROG_ERASE 77u
#define TID_P_PROG_PROGRAM 78u
#define TID_P_PROG_VERIFY 79u
#define TID_P_PROG_ISP 80u
#define TID_P_PROG_CAPACITY 81u
#define TID_P_PROG_ERASE_ALL 82u
#define TID_P_PROG_ERASE_WARN 83u
#define TID_P_PROG_WRITE_OK 84u
#define TID_P_PROG_VERIFY_OK 85u
#define TID_P_PROG_BYTE_OK 86u
#define TID_P_PROG_NOFLASH 87u
#define TID_P_PROG_LOOP_ERR 88u
#define TID_P_PROG_CHECK_WIRE 89u
#define TID_P_DMC_TITLE 90u
#define TID_P_DMC_ADDR 91u
#define TID_P_DMC_LEN 92u
#define TID_P_DMC_BCAST 93u
#define TID_P_DMC_UNICAST 94u
#define TID_P_DMC_HELLO 95u
#define TID_P_DMC_DATA 96u
#define TID_P_DMC_STATUS 97u
#define TID_P_DMC_RESET 98u
#define TID_P_DMC_CRC 99u
#define TID_P_DMC_RXERR 100u
#define TID_P_DMC_TXERR 101u
#define TID_P_DMC_NOLINK 102u
#define TID_P_DMC_LEN_LIMIT 103u
#define TID_P_DC_TITLE 104u
#define TID_P_DC_SIDE 105u
#define TID_P_DC_INPUT 106u
#define TID_P_DC_OUTPUT 107u
#define TID_P_DC_ON 108u
#define TID_P_DC_OFF 109u
#define TID_P_DC_SET 110u
#define TID_P_DC_WIN 111u
#define TID_P_DC_REF 112u
#define TID_P_DC_VOLT 113u
#define TID_P_DC_CURR 114u
#define TID_P_DC_POWR 115u
#define TID_P_DC_IN_RANGE 116u
#define TID_P_DC_OUT_RANGE 117u
#define TID_P_DC_PROTO 118u
#define TID_P_DC_SESSION 119u
#define TID_P_DC_SESSION_OPEN 120u
#define TID_P_DC_SESSION_CLOSED 121u
#define TID_P_DC_ADC_NOBIND 122u
#define TID_P_DIR_TITLE 123u
#define TID_P_DIR_CHAN 124u
#define TID_P_DIR_UART 125u
#define TID_P_DIR_I2C 126u
#define TID_P_DIR_PWM 127u
#define TID_P_DIR_PASSTHRU 128u
#define TID_P_DIR_RATE 129u
#define TID_P_DIR_LOOP 130u
#define TID_P_DIR_THROUGHPUT 131u
#define TID_P_DIR_OK 132u
#define TID_P_DIR_ERR 133u
#define TID_P_FAULT_TITLE 134u
#define TID_P_FAULT_PIN 135u
#define TID_P_FAULT_STATE 136u
#define TID_P_FAULT_OK 137u
#define TID_P_FAULT_NOLINK 138u
#define TID_P_FAULT_BAD 139u
#define TID_P_FAULT_SCAN 140u
#define TID_P_FAULT_INJECT 141u
#define TID_P_FAULT_CLEAR 142u
#define TID_P_FAULT_SUMMARY 143u
#define TID_P_FAULT_COUNT 144u
#define TID_P_SET_TITLE 145u
#define TID_P_SET_LANG 146u
#define TID_P_SET_TIME 147u
#define TID_P_SET_SNTP 148u
#define TID_P_SET_SAVE 149u
#define TID_P_SET_SAVED 150u
#define TID_P_SET_FACTORY 151u
#define TID_P_SET_FACTORY_WARN 152u
#define TID_P_SET_FACTORY_DONE 153u
#define TID_P_SET_SAVED_NVS 154u
#define TID_P_STA_TITLE 155u
#define TID_P_STA_WIFI 156u
#define TID_P_STA_IP 157u
#define TID_P_STA_RSSI 158u
#define TID_P_STA_UPTIME 159u
#define TID_P_STA_HEAP 160u
#define TID_P_STA_FLASH 161u
#define TID_P_STA_TIME 162u
#define TID_P_STA_LINKED 163u
#define TID_P_STA_LINKING 164u
#define TID_P_STA_NONET 165u
#define TID_P_ABOUT_TITLE 166u
#define TID_P_ABOUT_MODEL 167u
#define TID_P_ABOUT_NAME 168u
#define TID_P_ABOUT_BUILD 169u
#define TID_P_ABOUT_FLASH 170u
#define TID_P_ABOUT_SCREEN 171u
#define TID_P_ABOUT_BUTTONS 172u
#define TID_P_ABOUT_HW 173u
#define TID_M_HOME 174u
#define TID_M_PINS 175u
#define TID_M_WIZ 176u
#define TID_P_PINS_TITLE 177u
#define TID_P_PINS_PROFILE 178u
#define TID_P_PINS_SIG 179u
#define TID_P_PINS_GPIO 180u
#define TID_P_PINS_UNMAPPED 181u
#define TID_P_PROG_ISP_PID 182u
#define TID_P_PROG_ISP_VER 183u
#define TID_S_CONFIRM_HINT 184u
#define TID_S_STEP 185u
#define TID_S_DONE 186u
#define TID_S_ACTIVE 187u
#define TID_S_HINT_NAV 188u
#define TID_S_HINT_ACT 189u
#define TID_S_HINT_EDIT 190u

/** 取文案：TR(TID_S_OK)。
 *  ⚠️ 结果**不要长期缓存** —— 切语言后旧指针仍指向旧语言，
 *     会显示成「半中半英」。要缓存就存 idx，切语言时统一重刷。 */
#define TR(idx) ui_tr(idx)

/** 取文案 */
const char *ui_tr(uint16_t idx);

/** 按 key 查索引；查不到返回 0xFFFF（配合 UI_STR_MISSING 暴露问题） */
uint16_t ui_tr_id(const char *key);

#ifdef __cplusplus
}
#endif
