cmake_minimum_required(VERSION 3.16)

# 最小组件集: 只建 main + 依赖 (driver/esp_adc/esp_timer), 避免拉 mbedtls/证书包
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hw_pin_s3_test)
