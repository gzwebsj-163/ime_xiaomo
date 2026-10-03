/**
 * i18n.c / i18n.h — 多语言文案表（自动生成，⚠️ 勿手改）
 *
 * 由 tools/gen_i18n.py 从 main/i18n/strings.txt 生成。
 * 加文案的正确做法：改 strings.txt → 重跑 gen_i18n.py → 重跑 gen_font.py
 *                   （字体子集也依赖文案表！漏了 gen_font.py 会出现方块字）
 */

#include "i18n.h"
#include <stddef.h>

/* 运行时当前语言；0=中文（出厂默认） 1=English */
uint8_t g_ui_lang = 0;

/* 找不到的 key 走这里 —— 故意显示醒目文本而非空串，
 * 因为空串在屏上表现为「标签凭空消失」，排查成本极高。 */
const char *const UI_STR_MISSING = "\xE2\x9C\x97 [i18n MISSING]";
const char *const UI_LANG_NAME[2] = { "\xE4\xB8\xAD\xE6\x96\x87", "English" };

/* ---- 文案表：[语言][条目] ---- */
static const char *const UI_STR[2][191] = {
  {
    "确定", /* S_OK */
    "取消", /* S_CANCEL */
    "返回", /* S_BACK */
    "保存", /* S_SAVE */
    "已保存", /* S_SAVED */
    "是", /* S_YES */
    "否", /* S_NO */
    "开", /* S_ON */
    "关", /* S_OFF */
    "无", /* S_NONE */
    "自动", /* S_AUTO */
    "手动", /* S_MANUAL */
    "默认", /* S_DEFAULT */
    "错误", /* S_ERROR */
    "失败", /* S_FAIL */
    "通过", /* S_PASS */
    "就绪", /* S_READY */
    "处理中", /* S_BUSY */
    "等待", /* S_WAIT */
    "未知", /* S_UNKNOWN */
    "未检出", /* S_NONE_DETECTED */
    "确认操作", /* S_ARE_YOU_SURE */
    "确认", /* S_CONFIRM */
    "加载中", /* S_LOADING */
    "版本", /* S_VERSION */
    "合计", /* S_TOTAL */
    "远程调试器", /* W_BRAND */
    "正在启动", /* W_BOOTING */
    "长按返回跳过", /* W_SKIP */
    "首次使用设置", /* W_SETUP_FIRST */
    "重新设置向导", /* W_SETUP_AGAIN */
    "第 1 步 语言", /* W_STEP_LANG */
    "第 2 步 时间", /* W_STEP_TIME */
    "第 3 步 完成", /* W_STEP_DONE */
    "选择语言", /* W_LANG_TITLE */
    "中文", /* W_LANG_ZH */
    "English", /* W_LANG_EN */
    "设置时间", /* W_TIME_TITLE */
    "年", /* W_TIME_YEAR */
    "月", /* W_TIME_MONTH */
    "日", /* W_TIME_DAY */
    "时", /* W_TIME_HOUR */
    "分", /* W_TIME_MIN */
    "秒", /* W_TIME_SEC */
    "长按上键调整", /* W_TIME_UP */
    "长按下键调整", /* W_TIME_DOWN */
    "长按上键继续", /* W_TIME_NEXT */
    "时间已保存", /* W_TIME_SAVED */
    "设置完成", /* W_SETUP_DONE */
    "进入主菜单", /* W_ENTER_MENU */
    "探针检测", /* M_PROBE */
    "烧录器", /* M_FLASH */
    "DMC 链路", /* M_DMC */
    "DC 电源", /* M_DC */
    "直通控制", /* M_DIRECT */
    "故障诊断", /* M_FAULT */
    "系统设置", /* M_SETUP */
    "状态信息", /* M_STATUS */
    "关于", /* M_ABOUT */
    "探针检测", /* P_PROBE_TITLE */
    "档案", /* P_PROBE_PROFILE */
    "驱动", /* P_PROBE_DRIVER */
    "读标识", /* P_PROBE_JEDEC */
    "开始检测", /* P_PROBE_START */
    "厂商", /* P_PROBE_VENDOR */
    "器件", /* P_PROBE_DEV */
    "容量", /* P_PROBE_CAPACITY */
    "序列号", /* P_PROBE_SERIAL */
    "已检出", /* P_PROBE_FOUND */
    "未检出器件", /* P_PROBE_NO_CHIP */
    "检查跳线 供电 档位", /* P_PROBE_TIP_CHECK */
    "检查探针接触", /* P_PROBE_TIP_PROBE */
    "模拟", /* P_PROBE_SIM */
    "目标", /* P_PROG_TARGET */
    "驱动", /* P_PROG_DRIVER */
    "镜像", /* P_PROG_IMAGE */
    "读标识", /* P_PROG_READ_ID */
    "擦除", /* P_PROG_ERASE */
    "写入", /* P_PROG_PROGRAM */
    "校验", /* P_PROG_VERIFY */
    "同步", /* P_PROG_ISP */
    "读容量", /* P_PROG_CAPACITY */
    "整片擦除", /* P_PROG_ERASE_ALL */
    "擦除不可恢复", /* P_PROG_ERASE_WARN */
    "写入成功", /* P_PROG_WRITE_OK */
    "校验一致", /* P_PROG_VERIFY_OK */
    "逐字节一致", /* P_PROG_BYTE_OK */
    "未连接芯片", /* P_PROG_NOFLASH */
    "回环错误", /* P_PROG_LOOP_ERR */
    "检查接线与跳线", /* P_PROG_CHECK_WIRE */
    "DMC 链路", /* P_DMC_TITLE */
    "从机地址", /* P_DMC_ADDR */
    "载荷长度", /* P_DMC_LEN */
    "广播", /* P_DMC_BCAST */
    "单播", /* P_DMC_UNICAST */
    "握手", /* P_DMC_HELLO */
    "数据", /* P_DMC_DATA */
    "状态查询", /* P_DMC_STATUS */
    "复位", /* P_DMC_RESET */
    "校验自检", /* P_DMC_CRC */
    "接收错误", /* P_DMC_RXERR */
    "发送错误", /* P_DMC_TXERR */
    "无链路回应", /* P_DMC_NOLINK */
    "长度超限 上限 249", /* P_DMC_LEN_LIMIT */
    "DC 电源", /* P_DC_TITLE */
    "侧别", /* P_DC_SIDE */
    "输入", /* P_DC_INPUT */
    "输出", /* P_DC_OUTPUT */
    "上电", /* P_DC_ON */
    "断电", /* P_DC_OFF */
    "设定值", /* P_DC_SET */
    "窗口", /* P_DC_WIN */
    "参考预值", /* P_DC_REF */
    "电压", /* P_DC_VOLT */
    "电流", /* P_DC_CURR */
    "功率", /* P_DC_POWR */
    "在窗口内", /* P_DC_IN_RANGE */
    "超窗口", /* P_DC_OUT_RANGE */
    "供电协议", /* P_DC_PROTO */
    "会话", /* P_DC_SESSION */
    "已开会话", /* P_DC_SESSION_OPEN */
    "已关会话", /* P_DC_SESSION_CLOSED */
    "模拟值 非实测", /* P_DC_ADC_NOBIND */
    "直通控制", /* P_DIR_TITLE */
    "通道", /* P_DIR_CHAN */
    "串口", /* P_DIR_UART */
    "双线总线", /* P_DIR_I2C */
    "脉宽", /* P_DIR_PWM */
    "透传", /* P_DIR_PASSTHRU */
    "速率", /* P_DIR_RATE */
    "回环校验", /* P_DIR_LOOP */
    "吞吐", /* P_DIR_THROUGHPUT */
    "透传正常", /* P_DIR_OK */
    "透传错误", /* P_DIR_ERR */
    "故障诊断", /* P_FAULT_TITLE */
    "引脚", /* P_FAULT_PIN */
    "状态", /* P_FAULT_STATE */
    "正常", /* P_FAULT_OK */
    "未连接", /* P_FAULT_NOLINK */
    "异常", /* P_FAULT_BAD */
    "逐脚扫描", /* P_FAULT_SCAN */
    "注入故障", /* P_FAULT_INJECT */
    "清除故障", /* P_FAULT_CLEAR */
    "汇总", /* P_FAULT_SUMMARY */
    "数量", /* P_FAULT_COUNT */
    "系统设置", /* P_SET_TITLE */
    "语言", /* P_SET_LANG */
    "时间", /* P_SET_TIME */
    "网络校时", /* P_SET_SNTP */
    "保存设置", /* P_SET_SAVE */
    "设置已保存", /* P_SET_SAVED */
    "恢复出厂", /* P_SET_FACTORY */
    "将清除全部设置", /* P_SET_FACTORY_WARN */
    "已恢复出厂设置", /* P_SET_FACTORY_DONE */
    "配置已写入存储", /* P_SET_SAVED_NVS */
    "状态信息", /* P_STA_TITLE */
    "无线网络", /* P_STA_WIFI */
    "地址", /* P_STA_IP */
    "信号", /* P_STA_RSSI */
    "运行时间", /* P_STA_UPTIME */
    "剩余内存", /* P_STA_HEAP */
    "固件占用", /* P_STA_FLASH */
    "当前时间", /* P_STA_TIME */
    "已连接", /* P_STA_LINKED */
    "连接中", /* P_STA_LINKING */
    "未连接", /* P_STA_NONET */
    "关于", /* P_ABOUT_TITLE */
    "型号", /* P_ABOUT_MODEL */
    "产品名", /* P_ABOUT_NAME */
    "构建", /* P_ABOUT_BUILD */
    "固件", /* P_ABOUT_FLASH */
    "屏幕", /* P_ABOUT_SCREEN */
    "按键", /* P_ABOUT_BUTTONS */
    "硬件", /* P_ABOUT_HW */
    "主菜单", /* M_HOME */
    "引脚档案", /* M_PINS */
    "设置向导", /* M_WIZ */
    "引脚档案", /* P_PINS_TITLE */
    "当前档案", /* P_PINS_PROFILE */
    "信号", /* P_PINS_SIG */
    "GPIO", /* P_PINS_GPIO */
    "未映射", /* P_PINS_UNMAPPED */
    "芯片 ID", /* P_PROG_ISP_PID */
    "版本", /* P_PROG_ISP_VER */
    "长按上键确认", /* S_CONFIRM_HINT */
    "步骤", /* S_STEP */
    "完成", /* S_DONE */
    "当前", /* S_ACTIVE */
    "长按上键进入 长按下键返回", /* S_HINT_NAV */
    "长按上键执行", /* S_HINT_ACT */
    "长按上键切换", /* S_HINT_EDIT */
  },
  {
    "OK", /* S_OK */
    "Cancel", /* S_CANCEL */
    "Back", /* S_BACK */
    "Save", /* S_SAVE */
    "Saved", /* S_SAVED */
    "Yes", /* S_YES */
    "No", /* S_NO */
    "On", /* S_ON */
    "Off", /* S_OFF */
    "None", /* S_NONE */
    "Auto", /* S_AUTO */
    "Manual", /* S_MANUAL */
    "Default", /* S_DEFAULT */
    "Error", /* S_ERROR */
    "Fail", /* S_FAIL */
    "Pass", /* S_PASS */
    "Ready", /* S_READY */
    "Busy", /* S_BUSY */
    "Wait", /* S_WAIT */
    "Unknown", /* S_UNKNOWN */
    "Not Detected", /* S_NONE_DETECTED */
    "Are You Sure", /* S_ARE_YOU_SURE */
    "Confirm", /* S_CONFIRM */
    "Loading", /* S_LOADING */
    "Version", /* S_VERSION */
    "Total", /* S_TOTAL */
    "Remote Debugger", /* W_BRAND */
    "Booting", /* W_BOOTING */
    "Hold Back to Skip", /* W_SKIP */
    "First-Time Setup", /* W_SETUP_FIRST */
    "Run Setup Again", /* W_SETUP_AGAIN */
    "Step 1 of 3 Language", /* W_STEP_LANG */
    "Step 2 of 3 Date & Time", /* W_STEP_TIME */
    "Step 3 of 3 Finish", /* W_STEP_DONE */
    "Select Language", /* W_LANG_TITLE */
    "Chinese", /* W_LANG_ZH */
    "English", /* W_LANG_EN */
    "Set Date & Time", /* W_TIME_TITLE */
    "Year", /* W_TIME_YEAR */
    "Month", /* W_TIME_MONTH */
    "Day", /* W_TIME_DAY */
    "Hour", /* W_TIME_HOUR */
    "Minute", /* W_TIME_MIN */
    "Second", /* W_TIME_SEC */
    "Hold Up to Adjust", /* W_TIME_UP */
    "Hold Down to Adjust", /* W_TIME_DOWN */
    "Hold Up for Next", /* W_TIME_NEXT */
    "Time Saved", /* W_TIME_SAVED */
    "Setup Complete", /* W_SETUP_DONE */
    "Entering Menu", /* W_ENTER_MENU */
    "Probe Detect", /* M_PROBE */
    "Flasher", /* M_FLASH */
    "DMC Link", /* M_DMC */
    "DC Power", /* M_DC */
    "Direct Pass", /* M_DIRECT */
    "Fault Diag", /* M_FAULT */
    "System Setup", /* M_SETUP */
    "Status", /* M_STATUS */
    "About", /* M_ABOUT */
    "Probe Detection", /* P_PROBE_TITLE */
    "Profile", /* P_PROBE_PROFILE */
    "Driver", /* P_PROBE_DRIVER */
    "Read JEDEC", /* P_PROBE_JEDEC */
    "Start Detect", /* P_PROBE_START */
    "Vendor", /* P_PROBE_VENDOR */
    "Device", /* P_PROBE_DEV */
    "Capacity", /* P_PROBE_CAPACITY */
    "Serial", /* P_PROBE_SERIAL */
    "Detected", /* P_PROBE_FOUND */
    "No Device Found", /* P_PROBE_NO_CHIP */
    "Check Jumper Supply Mode", /* P_PROBE_TIP_CHECK */
    "Check Probe Contact", /* P_PROBE_TIP_PROBE */
    "Simulated", /* P_PROBE_SIM */
    "Target", /* P_PROG_TARGET */
    "Driver", /* P_PROG_DRIVER */
    "Image", /* P_PROG_IMAGE */
    "Read ID", /* P_PROG_READ_ID */
    "Erase", /* P_PROG_ERASE */
    "Program", /* P_PROG_PROGRAM */
    "Verify", /* P_PROG_VERIFY */
    "ISP Sync", /* P_PROG_ISP */
    "Read Size", /* P_PROG_CAPACITY */
    "Chip Erase", /* P_PROG_ERASE_ALL */
    "Erase Is Permanent", /* P_PROG_ERASE_WARN */
    "Write OK", /* P_PROG_WRITE_OK */
    "Verify OK", /* P_PROG_VERIFY_OK */
    "Byte Match", /* P_PROG_BYTE_OK */
    "No Flash Present", /* P_PROG_NOFLASH */
    "Loopback Error", /* P_PROG_LOOP_ERR */
    "Check Wiring Jumper", /* P_PROG_CHECK_WIRE */
    "DMC Link", /* P_DMC_TITLE */
    "Slave Addr", /* P_DMC_ADDR */
    "Payload Len", /* P_DMC_LEN */
    "Broadcast", /* P_DMC_BCAST */
    "Unicast", /* P_DMC_UNICAST */
    "Hello", /* P_DMC_HELLO */
    "Data", /* P_DMC_DATA */
    "Status", /* P_DMC_STATUS */
    "Reset", /* P_DMC_RESET */
    "CRC Self-Test", /* P_DMC_CRC */
    "RX Errors", /* P_DMC_RXERR */
    "TX Errors", /* P_DMC_TXERR */
    "No Link Response", /* P_DMC_NOLINK */
    "Length Over Limit 249 Max", /* P_DMC_LEN_LIMIT */
    "DC Power", /* P_DC_TITLE */
    "Polarity", /* P_DC_SIDE */
    "Input", /* P_DC_INPUT */
    "Output", /* P_DC_OUTPUT */
    "Power On", /* P_DC_ON */
    "Power Off", /* P_DC_OFF */
    "Setpoint", /* P_DC_SET */
    "Window", /* P_DC_WIN */
    "Reference", /* P_DC_REF */
    "Voltage", /* P_DC_VOLT */
    "Current", /* P_DC_CURR */
    "Power", /* P_DC_POWR */
    "In Window", /* P_DC_IN_RANGE */
    "Out Of Window", /* P_DC_OUT_RANGE */
    "Power Protocol", /* P_DC_PROTO */
    "Session", /* P_DC_SESSION */
    "Session Open", /* P_DC_SESSION_OPEN */
    "Session Closed", /* P_DC_SESSION_CLOSED */
    "Simulated Not Measured", /* P_DC_ADC_NOBIND */
    "Direct Pass", /* P_DIR_TITLE */
    "Channel", /* P_DIR_CHAN */
    "UART", /* P_DIR_UART */
    "I2C", /* P_DIR_I2C */
    "PWM", /* P_DIR_PWM */
    "Pass-Through", /* P_DIR_PASSTHRU */
    "Rate", /* P_DIR_RATE */
    "Loopback Test", /* P_DIR_LOOP */
    "Throughput", /* P_DIR_THROUGHPUT */
    "Pass-Through OK", /* P_DIR_OK */
    "Pass-Through Error", /* P_DIR_ERR */
    "Fault Diagnostics", /* P_FAULT_TITLE */
    "Pin", /* P_FAULT_PIN */
    "State", /* P_FAULT_STATE */
    "OK", /* P_FAULT_OK */
    "No Link", /* P_FAULT_NOLINK */
    "Bad", /* P_FAULT_BAD */
    "Pin-by-Pin Scan", /* P_FAULT_SCAN */
    "Inject Fault", /* P_FAULT_INJECT */
    "Clear Fault", /* P_FAULT_CLEAR */
    "Summary", /* P_FAULT_SUMMARY */
    "Count", /* P_FAULT_COUNT */
    "System Setup", /* P_SET_TITLE */
    "Language", /* P_SET_LANG */
    "Date & Time", /* P_SET_TIME */
    "Network Time", /* P_SET_SNTP */
    "Save Settings", /* P_SET_SAVE */
    "Settings Saved", /* P_SET_SAVED */
    "Factory Reset", /* P_SET_FACTORY */
    "Clears All Settings", /* P_SET_FACTORY_WARN */
    "Factory Settings Restored", /* P_SET_FACTORY_DONE */
    "Settings Written to Storage", /* P_SET_SAVED_NVS */
    "Status", /* P_STA_TITLE */
    "WiFi", /* P_STA_WIFI */
    "IP", /* P_STA_IP */
    "Signal", /* P_STA_RSSI */
    "Uptime", /* P_STA_UPTIME */
    "Free Heap", /* P_STA_HEAP */
    "Firmware Used", /* P_STA_FLASH */
    "Current Time", /* P_STA_TIME */
    "Connected", /* P_STA_LINKED */
    "Connecting", /* P_STA_LINKING */
    "Not Connected", /* P_STA_NONET */
    "About", /* P_ABOUT_TITLE */
    "Model", /* P_ABOUT_MODEL */
    "Product", /* P_ABOUT_NAME */
    "Build", /* P_ABOUT_BUILD */
    "Firmware", /* P_ABOUT_FLASH */
    "Screen", /* P_ABOUT_SCREEN */
    "Buttons", /* P_ABOUT_BUTTONS */
    "Hardware", /* P_ABOUT_HW */
    "Main Menu", /* M_HOME */
    "Pin Map", /* M_PINS */
    "Setup Wizard", /* M_WIZ */
    "Pin Map", /* P_PINS_TITLE */
    "Active Profile", /* P_PINS_PROFILE */
    "Signal", /* P_PINS_SIG */
    "GPIO", /* P_PINS_GPIO */
    "Unmapped", /* P_PINS_UNMAPPED */
    "Chip PID", /* P_PROG_ISP_PID */
    "Version", /* P_PROG_ISP_VER */
    "Hold Up to Confirm", /* S_CONFIRM_HINT */
    "Step", /* S_STEP */
    "Done", /* S_DONE */
    "Active", /* S_ACTIVE */
    "Hold Up Enter Hold Down Back", /* S_HINT_NAV */
    "Hold Up to Run", /* S_HINT_ACT */
    "Hold Up to Change", /* S_HINT_EDIT */
  }
};

/* ---- 运行时 key→索引（字符串比较；仅自检/调试用） ---- */
static const char *const UI_KEYS[] = {
    "S_OK", "S_CANCEL", "S_BACK", "S_SAVE", "S_SAVED", "S_YES", "S_NO", "S_ON", "S_OFF", "S_NONE", "S_AUTO", "S_MANUAL", "S_DEFAULT", "S_ERROR", "S_FAIL", "S_PASS", "S_READY", "S_BUSY", "S_WAIT", "S_UNKNOWN", "S_NONE_DETECTED", "S_ARE_YOU_SURE", "S_CONFIRM", "S_LOADING", "S_VERSION", "S_TOTAL", "W_BRAND", "W_BOOTING", "W_SKIP", "W_SETUP_FIRST", "W_SETUP_AGAIN", "W_STEP_LANG", "W_STEP_TIME", "W_STEP_DONE", "W_LANG_TITLE", "W_LANG_ZH", "W_LANG_EN", "W_TIME_TITLE", "W_TIME_YEAR", "W_TIME_MONTH", "W_TIME_DAY", "W_TIME_HOUR", "W_TIME_MIN", "W_TIME_SEC", "W_TIME_UP", "W_TIME_DOWN", "W_TIME_NEXT", "W_TIME_SAVED", "W_SETUP_DONE", "W_ENTER_MENU", "M_PROBE", "M_FLASH", "M_DMC", "M_DC", "M_DIRECT", "M_FAULT", "M_SETUP", "M_STATUS", "M_ABOUT", "P_PROBE_TITLE", "P_PROBE_PROFILE", "P_PROBE_DRIVER", "P_PROBE_JEDEC", "P_PROBE_START", "P_PROBE_VENDOR", "P_PROBE_DEV", "P_PROBE_CAPACITY", "P_PROBE_SERIAL", "P_PROBE_FOUND", "P_PROBE_NO_CHIP", "P_PROBE_TIP_CHECK", "P_PROBE_TIP_PROBE", "P_PROBE_SIM", "P_PROG_TARGET", "P_PROG_DRIVER", "P_PROG_IMAGE", "P_PROG_READ_ID", "P_PROG_ERASE", "P_PROG_PROGRAM", "P_PROG_VERIFY", "P_PROG_ISP", "P_PROG_CAPACITY", "P_PROG_ERASE_ALL", "P_PROG_ERASE_WARN", "P_PROG_WRITE_OK", "P_PROG_VERIFY_OK", "P_PROG_BYTE_OK", "P_PROG_NOFLASH", "P_PROG_LOOP_ERR", "P_PROG_CHECK_WIRE", "P_DMC_TITLE", "P_DMC_ADDR", "P_DMC_LEN", "P_DMC_BCAST", "P_DMC_UNICAST", "P_DMC_HELLO", "P_DMC_DATA", "P_DMC_STATUS", "P_DMC_RESET", "P_DMC_CRC", "P_DMC_RXERR", "P_DMC_TXERR", "P_DMC_NOLINK", "P_DMC_LEN_LIMIT", "P_DC_TITLE", "P_DC_SIDE", "P_DC_INPUT", "P_DC_OUTPUT", "P_DC_ON", "P_DC_OFF", "P_DC_SET", "P_DC_WIN", "P_DC_REF", "P_DC_VOLT", "P_DC_CURR", "P_DC_POWR", "P_DC_IN_RANGE", "P_DC_OUT_RANGE", "P_DC_PROTO", "P_DC_SESSION", "P_DC_SESSION_OPEN", "P_DC_SESSION_CLOSED", "P_DC_ADC_NOBIND", "P_DIR_TITLE", "P_DIR_CHAN", "P_DIR_UART", "P_DIR_I2C", "P_DIR_PWM", "P_DIR_PASSTHRU", "P_DIR_RATE", "P_DIR_LOOP", "P_DIR_THROUGHPUT", "P_DIR_OK", "P_DIR_ERR", "P_FAULT_TITLE", "P_FAULT_PIN", "P_FAULT_STATE", "P_FAULT_OK", "P_FAULT_NOLINK", "P_FAULT_BAD", "P_FAULT_SCAN", "P_FAULT_INJECT", "P_FAULT_CLEAR", "P_FAULT_SUMMARY", "P_FAULT_COUNT", "P_SET_TITLE", "P_SET_LANG", "P_SET_TIME", "P_SET_SNTP", "P_SET_SAVE", "P_SET_SAVED", "P_SET_FACTORY", "P_SET_FACTORY_WARN", "P_SET_FACTORY_DONE", "P_SET_SAVED_NVS", "P_STA_TITLE", "P_STA_WIFI", "P_STA_IP", "P_STA_RSSI", "P_STA_UPTIME", "P_STA_HEAP", "P_STA_FLASH", "P_STA_TIME", "P_STA_LINKED", "P_STA_LINKING", "P_STA_NONET", "P_ABOUT_TITLE", "P_ABOUT_MODEL", "P_ABOUT_NAME", "P_ABOUT_BUILD", "P_ABOUT_FLASH", "P_ABOUT_SCREEN", "P_ABOUT_BUTTONS", "P_ABOUT_HW", "M_HOME", "M_PINS", "M_WIZ", "P_PINS_TITLE", "P_PINS_PROFILE", "P_PINS_SIG", "P_PINS_GPIO", "P_PINS_UNMAPPED", "P_PROG_ISP_PID", "P_PROG_ISP_VER", "S_CONFIRM_HINT", "S_STEP", "S_DONE", "S_ACTIVE", "S_HINT_NAV", "S_HINT_ACT", "S_HINT_EDIT"
};

uint16_t ui_tr_id(const char *key)
{
    for (uint16_t i = 0; i < UI_STR_COUNT; i++) {
        const char *a = UI_KEYS[i], *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return i;
    }
    return 0xFFFFu;      /* 未命中 → 调用方显示 UI_STR_MISSING */
}

const char *ui_tr(uint16_t idx)
{
    if (idx >= UI_STR_COUNT || g_ui_lang >= UI_LANG_COUNT) return UI_STR_MISSING;
    return UI_STR[g_ui_lang][idx];
}
