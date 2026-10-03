idf_component_register(
    SRCS "app_main.c"
         # ---- hw_dc 模块本体 (从 ~/Desktop/xiaomo 原样拷入) + 真机 BSP + 验证桥 ----
         "hw_dc.c" "dc_hw.c" "dc_diag.c"
    INCLUDE_DIRS "."
    # driver     = GPIO/UART (真机 BSP 用)
    # esp_adc    = 片上 ADC oneshot + 校准
    # esp_timer  = esp_timer_get_time
    REQUIRES driver esp_adc esp_timer
)

# ADC 引脚体检 + 驱动回读对照（仅换板排查用，产品构建必须为 0）。
#   起因 = 本板 ADC 脚不能靠猜：LCD 脚被外拉钉住、弱上下拉在 ADC 脚不生效、
#          adc_oneshot 还会独占 pad 关掉本脚输出驱动。
#   用法: idf.py -DDC_PIN_SCAN=1 build flash
set(DC_PIN_SCAN 0 CACHE BOOL "ADC 引脚体检 + 驱动回读对照（换板排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE DC_PIN_SCAN=${DC_PIN_SCAN})
