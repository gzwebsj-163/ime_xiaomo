cmake_minimum_required(VERSION 3.16)

# 最小组件集: 只建 main + 依赖 (driver/esp_timer),
# 避免拉进 mbedtls/证书包 (本机 IDF python 环境 cffi/cryptography 架构不匹配,
# gen_crt_bundle 会失败; 本测试不用 TLS, 直接裁剪掉整条链路)
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hw_dmc_s3_test)
