idf_component_register(
    SRCS "app_main.c" "lcd_hw.c" "ui.c" "btn.c" "pin_probe.c" "shot_probe.c" "prog_ui_probe.c" "wifi_sta.c"
         # ---- 烧录器（2026-09-30）：hw_pin 原样搬入 + S3 真机 BSP + 后台 worker ----
         "hw_pin.c" "prog_hw.c" "prog_api.c"
         # ---- UI V2 P0（2026-10-01）：中文字体子集 + 多语言文案表 ----
         # ⚠️ i18n.c 与 fonts/*.c 都是**生成产物**（tools/gen_i18n.py、tools/gen_font.py），
         #    源真分别是 main/i18n/strings.txt 与同文件。改文案请改真源后重跑脚本，
         #    绝不手改这三个生成文件——手改会在下次生成时被无声覆盖。
         "i18n.c" "cjk_probe.c"
         # ---- UI V2 P0（2026-10-01）：NVS 配置层（判据「断电重启设置不丢」）----
         # ⚠️ 踩过的坑：回读对拍绝不能走 ui_cfg_init()（它会重置时间基准 s_base_us，
         #    时钟每被读一次就冻结一次）。ui_cfg.c 内已拆成无副作用的 nvs_read_raw。
         "ui_cfg.c"
         "fonts/ui_cjk_14.c" "fonts/ui_cjk_16.c"
    INCLUDE_DIRS "." "fonts"
    # esp_wifi/nvs_flash/esp_netif/esp_event = 真实 WiFi STA（2026-09-29 新增，替换原假数据 STATUS 页）
    # driver = GPIO/UART（烧录器真机 BSP 用）
    # v3 UI 新增依赖：esp_app_format=esp_app_get_description()（关于页固件版本/构建时间）
    #                 spi_flash=esp_flash_get_size()（状态页/关于页 Flash 容量）
    #                 esp_netif=esp_netif_sntp（设置页「网络校时」）
    REQUIRES esp_lcd driver esp_timer esp_wifi nvs_flash esp_netif esp_event esp_app_format spi_flash
)

# 合成按键事件自检：1 = 启动后自动注入一串按键事件验证链路（仅调试用）
#                       产品构建必须为 0
target_compile_definitions(${COMPONENT_LIB} PRIVATE BTN_SELFTEST=0)

# 竞态放大器：1 = 故意用「旧顺序」（刷新任务先启动）+ 在 ui_init 建对象之间让出 CPU，
#             把「刷新任务 vs ui_init」的碰撞窗口放大到必然，用于对竞态做因果实证。
#             只用于排查，产品构建必须为 0。
#             用法：idf.py -DRACE_AMPLIFY=1 build
set(RACE_AMPLIFY 0 CACHE BOOL "放大 LVGL 刷新任务 vs ui_init 竞态（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE RACE_AMPLIFY=${RACE_AMPLIFY})

# 引脚可用性探针：1 = 启动时打印 GPIO33~37 保留状态 + PSRAM 完整性对拍（仅排查用）
#                 用法：idf.py -DPIN_PROBE=1 build
set(PIN_PROBE 0 CACHE BOOL "按键引脚可用性探针（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PIN_PROBE=${PIN_PROBE})

# 探针被测脚：0 = 用 IO35/36（用户指定，PSRAM 占用脚）；1 = 用 IO18（空闲脚，对照组）
#              只有「换引脚再跑一遍」才能区分「引脚的问题」和「探针自己的问题」
set(PROBE_TEST_FREE_PIN 0 CACHE BOOL "探针改用空闲脚 IO18 做对照组")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROBE_TEST_FREE_PIN=${PROBE_TEST_FREE_PIN})

# 屏幕渲染 dump 探针：1 = 用 lv_snapshot 把 9 页逐屏 dump 到串口，本地还原成 PNG（仅排查用）
#                     让「布局有没有压字/错位」从"只能靠人眼看"变成"能自证"。
#                     需要同时打开 CONFIG_LV_USE_SNAPSHOT。
#                     用法：idf.py -DSHOT_PROBE=1 build && ./tools/shot_fetch.py
set(SHOT_PROBE 0 CACHE BOOL "屏幕渲染 dump 探针（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SHOT_PROBE=${SHOT_PROBE})

# 中文字体上屏验证探针：1 = ①启动时自检 174 条文案×2语言在字体里无缺字
#                        ②建一个中文字体冒烟屏（配合 SHOT_PROBE 抓屏 OCR）
# 起因 = 字形数组是 static，会被 --gc-sections 回收。踩过一次：编译零错误、
#        固件一字节没变、nm 查不到符号，却看起来一切正常。编译不能证上屏。
# 用法：idf.py -DCJK_SMOKETEST=1 -DSHOT_PROBE=1 -DSHOT_PAGE_FIRST=99 \
#            -DSHOT_PAGE_LAST=99 build flash && ./tools/shot_fetch.py
set(CJK_SMOKETEST 0 CACHE BOOL "中文字体上屏验证探针（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE CJK_SMOKETEST=${CJK_SMOKETEST})

# NVS 配置层「写入回读逐位对拍」自检：1 = 启动时跑 ui_cfg_selftest（4 项），
#   跑完自动还原原配置。P0 判据「断电重启设置不丢」的机器可验手段。
#   ⚠️ 本项目踩过 CMake `CACHE BOOL` 粘住上次 -D 值的坑：产品构建必须显式传
#      -DUI_CFG_SELFTEST=0，并用 `grep UI_CFG_SELFTEST:BOOL build/CMakeCache.txt` 复核。
# 用法：idf.py -DUI_CFG_SELFTEST=1 build flash && python3 tools/boot_cap.py out.log 25
set(UI_CFG_SELFTEST 0 CACHE BOOL "NVS 配置层写入回读对拍自检（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_CFG_SELFTEST=${UI_CFG_SELFTEST})

# 死代码告警：只开 -Wunused-function（不做 -Wall -Wextra，存量代码噪音太大）。
# 起因 = 2026-10-01 踩过：修 memcmp 时新写了 cfg_eq() 却忘了换调用点，
# 编译零错误、烧录成功、日志照打、fails=4 —— 「编译通过」完全掩盖了
# 「修复没被使用」。开了这个开关，编译器会直接指着 cfg_eq 报未使用。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wunused-function)

# 跨重启持久化探针：1 = 开机时 ui_cfg_persist_probe()。
#   ⚠️ 必须跑两轮：第1轮写入 → 硬复位 → 第2轮校验。selftest 只证单次开机内一致，
#      证不了掉电；这个才是 P0 判据「断电重启设置不丢」的真形态。
#   用法：idf.py -DUI_CFG_PERSIST_TEST=1 build flash   (第1轮)
#         python3 tools/cap_reset.py                    (硬复位)
#         python3 tools/boot_cap.py out.log 22          (第2轮看 PASS)
set(UI_CFG_PERSIST_TEST 0 CACHE BOOL "NVS 跨重启持久化探针（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_CFG_PERSIST_TEST=${UI_CFG_PERSIST_TEST})

# 探针只抓哪几页（默认 0..9 全抓）。配合 SHOT_PROBE 用：
#   只抓第 9 页 → idf.py -DSHOT_PROBE=1 -DSHOT_PAGE_FIRST=9 -DSHOT_PAGE_LAST=9 build
set(SHOT_PAGE_FIRST 0 CACHE STRING "shot 探针起始页")
set(SHOT_PAGE_LAST  9 CACHE STRING "shot 探针结束页")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SHOT_PAGE_FIRST=${SHOT_PAGE_FIRST} SHOT_PAGE_LAST=${SHOT_PAGE_LAST})

# 烧录器后端自检：1 = 启动时自动把「UI 走的那条 API 路径」跑一遍（SIM 全链真跑 +
#                 REAL 模式必须诚实失败），打 PASS/FAIL 到串口（仅排查用，产品必须为 0）。
#                 起因 = UI 操作要按物理键，没法脚本化 → 把「后端通不通」变成机器可验。
#                 用法：idf.py -DPROG_SELFTEST=1 build flash && python3 tools/boot_cap.py out.log 20
set(PROG_SELFTEST 0 CACHE BOOL "烧录器后端启动自检（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROG_SELFTEST=${PROG_SELFTEST})

# WiFi 密码候选探测：1 = 依次把候选密码灌进驱动，谁先拿到 IP 就锁定谁并落 NVS（仅排查用）
#                     起因 = 用户给的密码末尾带「..」，疑似省略号。产品构建必须为 0。
#                     用法：idf.py -DWIFI_PASS_PROBE=1 build flash
set(WIFI_PASS_PROBE 0 CACHE BOOL "WiFi 密码候选探测（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_PASS_PROBE=${WIFI_PASS_PROBE})

# PROG UI 写路径自检：1 = 启动后自动把「按键 → 烧录后端」这条**写路径**跑一遍
#   （注入合成按键事件，走真实事件队列 → handle_buttons → prog_page_key → prog_api_start），
#   覆盖 SIM 的 READ ID/ERASE(?)/PROGRAM/VERIFY + 切 REAL 后的**诚实失败**，全程留串口证据。
#   起因 = 后端自检(PROG_SELFTEST)证的是「程序直接调 API 跑得对」、shot 探针证的是「数据能上屏」，
#          唯独「用户按键真的把后端开跑」这条写路径没法脚本化、只能人肉按键 → 无法自证。
#   产品构建必须为 0。用法：idf.py -DPROG_UI_TEST=1 build flash && python3 ../tools/boot_cap.py pui.log 30
set(PROG_UI_TEST 0 CACHE BOOL "烧录器 UI 写路径自检（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROG_UI_TEST=${PROG_UI_TEST})
