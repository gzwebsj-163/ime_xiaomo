idf_component_register(
    SRCS "app_main.c"
         # ---- hw_pin 模块本体 (从 ~/Desktop/xiaomo 原样拷入) ----
         "hw_pin.c"
         # ---- 真机 BSP (ESP32-S3 GPIO / SPI / UART / LEDC+ADC) ----
         "pin_hw.c"
         # ---- 测试仪器: UART2 上的 AN3155 STM32 器件模型 ----
         "pin_target.c"
         # ---- 验证桥: Phase A/B/C/L0 ----
         "pin_diag.c"
         # ---- 判别性实验: SPI 侧 GPIO Matrix A/B ----
         "pin_spi_ab.c"
         # ---- G2 失灵根因诊断: 方法 × 脚 交叉矩阵 ----
         "pin_g2_diag.c"
    INCLUDE_DIRS "."
    # driver     = GPIO/SPI/UART/LEDC (真机 BSP 用)
    # esp_adc    = 片上 ADC oneshot + 校准 (L0 反馈通道)
    # esp_timer  = esp_timer_get_time
    REQUIRES driver esp_adc esp_timer
)

# 引脚体检（换板排查用，产品构建必须为 0）。
set(PIN_PIN_SCAN 0 CACHE BOOL "引脚体检（换板排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIN_PIN_SCAN=${PIN_PIN_SCAN})

# SPI 侧 GPIO Matrix A/B 受控实验（唯一开关，默认关）。
# ⚠️ 用独立 build 目录 + 显式传 -DPIN_SPI_AB_ONLY=1；
#    沿用旧 build 目录时该 CACHE 值会粘住上一次，导致「以为在跑 A/B
#    其实跑的是 Phase A/B/C」——本项目已因 CMake CACHE 粘值栽过。
set(PIN_SPI_AB_ONLY 0 CACHE BOOL "只跑 SPI matrix A/B 裁决实验")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIN_SPI_AB_ONLY=${PIN_SPI_AB_ONLY})

# G2 失灵根因诊断（方法 × 脚 交叉矩阵）。
# ⚠️ 同样必须用独立 build 目录 + 显式传 -DPIN_G2_DIAG=1，CACHE 会粘住上次的值。
set(PIN_G2_DIAG 0 CACHE BOOL "只跑 G2 根因诊断矩阵")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIN_G2_DIAG=${PIN_G2_DIAG})
