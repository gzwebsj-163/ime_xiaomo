idf_component_register(
    SRCS "app_main.c"
         # ---- hw_dmc 模块本体 (从 ~/Desktop/xiaomo 原样拷入) + 真机 BSP + 验证桥 ----
         "hw_dmc.c" "dmc_hw.c" "dmc_diag.c" "dmc_resident.c" "dmc_slave.c" "dmc_cable.c"
    INCLUDE_DIRS "."
    # driver    = UART1 (主机真机 BSP) + UART2 (板载从机仿真器)
    # esp_timer = esp_timer_get_time (now_ms 时基)
    REQUIRES driver esp_timer
)

# ---- 运行模式开关: 1=常驻建链 (默认) / 0=一次性 A/B 验证 ----
# ⚠️ 用 STRING CACHE 而非 BOOL: BOOL 类型的 -D 会被 CMake 缓存粘住
#    （上一轮 -DPROG_SELFTEST=1 残留导致「重编产品版」仍得自检版）。
#    纪律 = 构建后必须 grep 本工程的 CMakeCache.txt 复核实际生效值。
set(DMC_MODE_RESIDENT "1" CACHE STRING "hw_dmc: 1=常驻建链 0=一次性验证")

# ---- 板载从设备仿真器（补 UP 路径证据）----
#   ON   : 1=装 0=不装
#   LINK : 0=GPIO-Matrix 内部互连(免接线)
#          1=外部跳线(IO17→IO9 / IO8→IO18)
#   2=IOMUX 双口 + 2 根跳线(TX↔RX 交叉: IO17↔IO44 / IO43↔IO18)  ← 2026-10-02 实测后
#             **S3 上唯一结构性可行**的方案：受控 A/B 证明 matrix 出向对 UART TX
#             无效（IO17 采样 1974/7000 vs IO9 采样 0/7000），且 S3 仅 UART0(IO43/44)
#             与 UART1(IO17/18) 有 IOMUX 专属脚（uart_pins.h: U2TXD_GPIO_NUM=-1）。
#             console 走 USB-Serial-JTAG 不占 UART0 引脚，故 UART0 可当从机。
#             详见 dmc_slave.h 的 LINK==2 分支。
#   MODE : 0=正常应答 1=静默(阴性对照) 2=坏CRC(完整性故障注入)
set(DMC_SLAVE_ON   "1" CACHE STRING "板载从机仿真器 1=开 0=关")
set(DMC_SLAVE_LINK "0" CACHE STRING "从机接线 0=GPIO-Matrix内部 1=外部跳线")
set(DMC_SLAVE_MODE "0" CACHE STRING "从机应答 0=正常 1=静默 2=坏CRC")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           DMC_MODE_RESIDENT=${DMC_MODE_RESIDENT}
                           DMC_SLAVE_ON=${DMC_SLAVE_ON}
                           DMC_SLAVE_LINK=${DMC_SLAVE_LINK}
                           DMC_SLAVE_MODE=${DMC_SLAVE_MODE})
