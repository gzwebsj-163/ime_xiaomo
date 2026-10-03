idf_component_register(
    SRCS "app_main.c"
         # ---- hw_usbpd 模块本体 + selftest (从 ~/Desktop/xiaomo/usbpd 原样拷入) ----
         "hw_usbpd.c" "hw_usbpd_selftest.c"
         # ---- 真机 BSP + 验证桥 ----
         "usbpd_hw.c" "usbpd_diag.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_timer
)

# ADC 引脚体检 (仅换板排查用, 产品/常规测试构建必须为 0)。
#   起因 = hw_dc 家族实测: 本板 ADC 脚不能靠猜, 且 adc_oneshot 会独占 pad。
#   用法: idf.py -DUSBPD_PIN_SCAN=1 build flash
set(USBPD_PIN_SCAN 0 CACHE BOOL "ADC 引脚体检（换板排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE USBPD_PIN_SCAN=${USBPD_PIN_SCAN})
