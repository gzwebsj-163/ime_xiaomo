cmake_minimum_required(VERSION 3.16)

# 最小组件集: 只建 main + 依赖 (driver/esp_adc/esp_timer)。
# 裁掉 mbedtls 整条链路 —— 本机 IDF python 环境 cffi/cryptography 架构不匹配,
# gen_crt_bundle 会失败; 本测试不用 TLS。与 hw_dc_s3_test 同款处置。
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hw_usbpd_s3_test)
