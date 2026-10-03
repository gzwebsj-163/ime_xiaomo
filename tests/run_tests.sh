#!/bin/bash
# xiaomo 自动化回归测试
# 用法: bash tests/run_tests.sh   (或 make test)
#
# 覆盖:
#   A) .mo 解释器层 (examples/*.mo vs tests/expected/*.txt)
#   B) Kills 字节码内核层 (内嵌 demo: 循环/序列化/CALL-RET/递归)

set -u
cd "$(dirname "$0")/.."
BIN=./xiaomo
PASS=0
FAIL=0
FAILED_NAMES=()

green() { printf "\033[32m%s\033[0m\n" "$*"; }
red()   { printf "\033[31m%s\033[0m\n" "$*"; }

echo "================  A. .mo 解释器回归 ================"
for mo in examples/*.mo; do
    name=$(basename "$mo" .mo)
    exp="tests/expected/$name.txt"
    if [ ! -f "$exp" ]; then
        red "  [SKIP] $name (无期望输出 $exp)"
        continue
    fi
    actual=$("$BIN" run "$mo" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        red "  [FAIL] $name (运行错误 rc=$rc)"
        echo "$actual"
        FAIL=$((FAIL+1)); FAILED_NAMES+=("$name")
    elif [ "$actual" == "$(cat "$exp")" ]; then
        green "  [PASS] $name"
        PASS=$((PASS+1))
    else
        red "  [FAIL] $name (输出不符)"
        echo "  --- 期望 ---"; cat "$exp" | sed 's/^/    /'
        echo "  --- 实际 ---"; echo "$actual" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("$name")
    fi
done

echo ""
echo "================  B. Kills 字节码内核回归 ================"
kvm_out=$("$BIN" kvm - 2>&1)
for probe in "720" "5" "120"; do
    if echo "$kvm_out" | grep -q "$probe"; then
        green "  [PASS] Kills 检测到输出: $probe"
        PASS=$((PASS+1))
    else
        red "  [FAIL] Kills 缺少输出: $probe (递归/CALL/循环可能损坏)"
        FAIL=$((FAIL+1)); FAILED_NAMES+=("kvm:$probe")
    fi
done

echo ""
echo "================  C. mo2kbc 编译器一致性 ================"
for mo in examples/*.mo; do
    name=$(basename "$mo" .mo)
    # 张量算子示例 (mlp*/train_*) 使用解释器层专属的原生张量内核,
    # mo2kbc 编译层暂不支持, 故不参与「编译输出与解释器一致」比对
    # (保持两层回归各自独立)。
    # openclaw_interact 使用 input_wait/read 交互 FFI, 编译层执行会
    # 阻塞等输入, 非交互环境无法比对, 同样跳过。
    # hwdev_test 使用 hw_dev() 字节码内置 (OP_HW_DEV_CALL, 解释器
    # 不含该调用, 同 linux_* 先例), 由 C 组 hwdev 块独立覆盖。
    # hwfault_test 使用 hw_fault() 字节码内置 (OP_HW_FAULT_CALL, 同先例),
    # 由 C 组 hwfault 块独立覆盖。
    # hwcore_test 使用 hw_core() 字节码内置 (OP_HW_CORE_CALL, 同先例),
    # 由 C 组 hwcore 块独立覆盖。
    # hwmain_test 使用 hw_main() 字节码内置 (OP_HW_MAIN_CALL, 同先例),
    # 由 C 组 hwmain 块独立覆盖。
    # wdbg_test 使用 hw_wdbg() 字节码内置 (OP_HW_WDBG_CALL, 同先例),
    # 由 C 组 wdbg 块独立覆盖。
    # flash_test 使用 hw_flash() 字节码内置 (OP_HW_FLASH_CALL, 同先例),
    # 由 C 组 flash 块独立覆盖。
    # pin_test 使用 hw_pin() 字节码内置 (OP_HW_PIN_CALL, 同先例),
    # 由 C 组 pin 块独立覆盖。
    # dc_test / dc_proto_test 使用 hw_dc() 字节码内置 (OP_HW_DC_CALL, 同先例),
    # 由 C 组 hwdc / hwdc proto 块独立覆盖。
    # dmc_test 使用 hw_dmc() 字节码内置 (OP_HW_DMC_CALL, 同先例),
    # 由 C 组 dmc / dmc .mo→kbc 块独立覆盖。
    case "$name" in
      mlp*|train_*|nd_tensor_test|linux_boot|openclaw_interact|hwdev_test|hwfault_test|hwcore_test|hwmain_test|wdbg_test|flash_test|pin_test|dc_test|dc_proto_test|dmc_test)
        echo "  [SKIP] mo2kbc $name (内核扩展专属: 张量算子/真Linux内核)"
        continue
        ;;
    esac
    interp=$("$BIN" run "$mo" 2>&1)
    comp=$("$BIN" mo2kbc "$mo" 2>&1)
    if [ "$interp" == "$comp" ]; then
        green "  [PASS] mo2kbc $name (编译输出与解释器一致)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] mo2kbc $name (编译输出与解释器不一致)"
        echo "  --- 解释器 ---"; echo "$interp" | sed 's/^/    /'
        echo "  --- 编译器 ---"; echo "$comp" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("mo2kbc:$name")
    fi
done

echo ""
echo "================  C. OEM 设备签名层 (hw_oem 熔丝) ================"
sig_out=$("$BIN" sig 2>&1)
if echo "$sig_out" | grep -q "0x5849414f4d4f3031" && echo "$sig_out" | grep -q "OK" && echo "$sig_out" | grep -q "REJECTED"; then
    green "  [PASS] sig (身份卡: hex=0x5849414f4d4f3031, VM验证OK, 重复烧录REJECTED)"
    PASS=$((PASS+1))
else
    red "  [FAIL] sig (身份卡输出异常)"
    echo "$sig_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("sig")
fi

sigp_out=$("$BIN" sigprobe 2>&1); sigp_rc=$?
if [ $sigp_rc -eq 0 ] && echo "$sigp_out" | grep -q "UNCHANGED" && echo "$sigp_out" | grep -qE "violations = [1-9]"; then
    green "  [PASS] sigprobe (越权写 R127 被步末写屏蔽回滚, violations>=1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] sigprobe (rc=$sigp_rc)"
    echo "$sigp_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("sigprobe")
fi

# ---- hw_dev 设备命令分发 (2026-09-07) ----
# 2b) i2c/pwm 外设: macOS 无节点=0x03 (IO错), Linux 真机=0x00 (枚举成功) → 双平台兼容断言
hw_out=$("$BIN" hwdev 2>&1); hw_rc=$?
if [ $hw_rc -eq 0 ] && echo "$hw_out" | grep -q "dispatch echo       = 0x00" \
   && echo "$hw_out" | grep -q "result=\[hello\]" \
   && echo "$hw_out" | grep -qE "dispatch i2c list   = 0x0(0|3)" \
   && echo "$hw_out" | grep -qE "dispatch pwm list   = 0x0(0|3)" \
   && echo "$hw_out" | grep -q "register led        = 0" \
   && echo "$hw_out" | grep -q "register led again  = -2" \
   && echo "$hw_out" | grep -q "dispatch led        = 0x2A" \
   && echo "$hw_out" | grep -q "hook 后 dispatch led= 0xFF"; then
    green "  [PASS] hwdev (ctrl两态/静态分发+结果通道/i2c-pwm外设/动态注册/撞名拦截/hook释放)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdev (能力卡输出异常 rc=$hw_rc)"
    echo "$hw_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdev")
fi

# hw_dev 一次性分发: 命中(0x00, exit 0) / 未命中(0xFF, exit 1)
hw1_out=$("$BIN" hwdev $'echo\n\r hello' 2>&1); hw1_rc=$?
hw2_out=$("$BIN" hwdev $'bogus\n\r ' 2>&1); hw2_rc=$?
if [ $hw1_rc -eq 0 ] && echo "$hw1_out" | grep -q "0x00" \
   && [ $hw2_rc -eq 1 ] && echo "$hw2_out" | grep -q "0xFF"; then
    green "  [PASS] hwdev one-shot (echo命中rc=0/exit0, bogus未命中rc=255/exit1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdev one-shot (rc1=$hw1_rc rc2=$hw2_rc)"
    echo "$hw1_out" | sed 's/^/    /'; echo "$hw2_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdev oneshot")
fi

# hw_dev .mo → kbc 端到端: hw_dev()内置 → OP_HW_DEV_CALL → hw_dev_dispatch
# 双参 imm 编码: hw_dev("i2c\n\r read ", 99) → 拼接 "read 99" → h=99 无效 → rc=4
#   (拼接失败则 params 缺参=rc 1, rc=4 即 imm 动态拼接生效的铁证)
hwm_out=$("$BIN" mo2kbc examples/hwdev_test.mo 2>&1)
if echo "$hwm_out" | grep -q "echo  rc=0" && echo "$hwm_out" | grep -q "self  rc=0" \
   && echo "$hwm_out" | grep -q "bogus rc=255" \
   && echo "$hwm_out" | grep -q "i2c99 rc=4" \
   && echo "$hwm_out" | grep -q "echo42 rc=0"; then
    green "  [PASS] hwdev .mo→kbc (hw_dev()内置+双参imm动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdev .mo→kbc"
    echo "$hwm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdev mo2kbc")
fi

# ---- hw_fault TFT 模组故障诊断 (2026-09-24) ----
fl_out=$("$BIN" hwfault 2>&1); fl_rc=$?
if [ $fl_rc -eq 0 ] && echo "$fl_out" | grep -q "golden OK" \
   && echo "$fl_out" | grep -q "scan=0 faults" \
   && echo "$fl_out" | grep -q "inject P7=VDD_LOW -> scan = 1" \
   && echo "$fl_out" | grep -q "inject P4=SPI_NOACK -> scan = 1" \
   && echo "$fl_out" | grep -q "clear -> scan = 0"; then
    green "  [PASS] hwfault (能力卡/黄金校验和/注入演示)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwfault (能力卡输出异常 rc=$fl_rc)"
    echo "$fl_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwfault")
fi

# hwfault 自检 (黄金锁定/全类别注入/scan计数/命令分发)
fls_out=$("$BIN" hwfault selftest 2>&1); fls_rc=$?
if [ $fls_rc -eq 0 ] && echo "$fls_out" | grep -q "all PASS"; then
    green "  [PASS] hwfault selftest (黄金锁定/全类别注入/scan计数/命令分发)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwfault selftest (rc=$fls_rc)"
    echo "$fls_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwfault selftest")
fi

# hwfault 一次性: 健康 P7=NONE exit0 (注入 VDD_LOW 后 P7=0x02 exit1)
flp_out=$("$BIN" hwfault pin 7 2>&1); flp_rc=$?
if [ $flp_rc -eq 0 ] && echo "$flp_out" | grep -q "NONE"; then
    green "  [PASS] hwfault pin (健康 P7=NONE exit0)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwfault pin (rc=$flp_rc)"
    echo "$flp_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwfault pin")
fi

# hwfault .mo → kbc 端到端: hw_fault()内置 → OP_HW_FAULT_CALL → hw_fault_cmd
# 双参 imm 编码: hw_fault("pin ", 7) → 拼接 "pin 7" → 2 (VDD_LOW);
#   (拼接失败则 strtol 空串 → -1 回绕 255, rc=2 即 imm 动态拼接生效铁证)
flm_out=$("$BIN" mo2kbc examples/hwfault_test.mo 2>&1)
if echo "$flm_out" | grep -q "scan0 rc=0" && echo "$flm_out" | grep -q "inject rc=0" \
   && echo "$flm_out" | grep -q "scan1 rc=1" && echo "$flm_out" | grep -q "pin7  rc=2" \
   && echo "$flm_out" | grep -q "pin7v rc=2" && echo "$flm_out" | grep -q "scan2 rc=0"; then
    green "  [PASS] hwfault .mo→kbc (hw_fault()内置+双参imm动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwfault .mo→kbc"
    echo "$flm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwfault mo2kbc")
fi

# ---- hw_core 内核 DNA 编码层 (2026-09-28) ----
co_out=$("$BIN" core 2>&1); co_rc=$?
if [ $co_rc -eq 0 ] && echo "$co_out" | grep -q "golden OK" \
   && echo "$co_out" | grep -q "count=10" \
   && echo "$co_out" | grep -q "slot 3 = 0x0080 -> used = 1" \
   && echo "$co_out" | grep -q "free 3  -> used = 0"; then
    green "  [PASS] hwcore (能力卡/黄金校验和/槽位装载演示)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwcore (能力卡输出异常 rc=$co_rc)"
    echo "$co_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwcore")
fi

# hwcore 自检 (黄金锁定/表序对拍/槽位生命周期/命令分发)
cos_out=$("$BIN" core selftest 2>&1); cos_rc=$?
if [ $cos_rc -eq 0 ] && echo "$cos_out" | grep -q "all PASS"; then
    green "  [PASS] hwcore selftest (黄金锁定/表序对拍/槽位生命周期/命令分发)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwcore selftest (rc=$cos_rc)"
    echo "$cos_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwcore selftest")
fi

# hwcore .mo → kbc 端到端: hw_core()内置 → OP_HW_CORE_CALL → hw_core_cmd
# 双参 imm 编码: hw_core("idx ", 3) → 拼接 "idx 3" → 221 (0x00DD SNG);
#   (拼接失败则 strtol 空串/越界 → -1 回绕 255, rc=221 即 imm 动态拼接生效铁证)
com_out=$("$BIN" mo2kbc examples/hwcore_test.mo 2>&1)
if echo "$com_out" | grep -q "count rc=10" && echo "$com_out" | grep -q "ok    rc=1" \
   && echo "$com_out" | grep -q "idx0  rc=3567" && echo "$com_out" | grep -q "idx9  rc=4093" \
   && echo "$com_out" | grep -q "idx3v rc=221" && echo "$com_out" | grep -q "slot  rc=0" \
   && echo "$com_out" | grep -q "used1 rc=1" && echo "$com_out" | grep -q "free  rc=0" \
   && echo "$com_out" | grep -q "used2 rc=0"; then
    green "  [PASS] hwcore .mo→kbc (hw_core()内置+双参imm动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwcore .mo→kbc"
    echo "$com_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwcore mo2kbc")
fi

# ---- hw_main 家族总调度 (2026-09-28) ----
mn_out=$("$BIN" main 2>&1); mn_rc=$?
if [ $mn_rc -eq 0 ] && echo "$mn_out" | grep -q "golden : 0xA57E74DF OK" \
   && echo "$mn_out" | grep -q "\[0\] ASR" && echo "$mn_out" | grep -q "\[7\] TOKEN" \
   && echo "$mn_out" | grep -q "\[8\] USBPD" \
   && echo "$mn_out" | grep -q "cls=0x0098" && echo "$mn_out" | grep -q "cls=0x0EFD" \
   && echo "$mn_out" | grep -q "cls=0x5044"; then
    green "  [PASS] hwmain (类注册表能力卡/黄金校验和/类ID对拍)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwmain (能力卡输出异常 rc=$mn_rc)"
    echo "$mn_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwmain")
fi

# hwmain 自检 (黄金锁定/类ID↔表序双向对拍/探针缓存生命周期/命令分发)
mns_out=$("$BIN" main selftest 2>&1); mns_rc=$?
if [ $mns_rc -eq 0 ] && echo "$mns_out" | grep -q "all PASS"; then
    green "  [PASS] hwmain selftest (黄金锁定/双向对拍/探针缓存/命令分发)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwmain selftest (rc=$mns_rc)"
    echo "$mns_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwmain selftest")
fi

# hwmain .mo → kbc 端到端: hw_main()内置 → OP_HW_MAIN_CALL → hw_main_cmd
# 双参 imm 编码: hw_main("probe ", 152) → 拼接 "probe 152" → CORE 探针 0;
#   (拼接失败则 find 152 落空 → -1 回绕 255, rc=0 即 imm 动态拼接生效铁证)
# ⚠️ 2026-10-03 sum 期望值由 1605203290 改为 -1518439201, 这不是"跟着现状改数":
#   登记 USBPD 后黄金 0x5FAD755A → 0xA57E74DF, 新值跨 2^31, (int) 忠实回传为负。
#   同日查明 cmd 出口曾对 32 位 FNV 掩 0x7FFFFFFF (旧黄金 <2^31 时是静默空操作),
#   掩码一旦生效两出口就给出不同值 —— 与 hw_dmc.c:1150 记录的"曾踩"同型, 已修。
#   断言负数恰恰让本项具备判别力: 谁再塞回掩码, 629044447 ≠ -1518439201 即报红。
mnm_out=$("$BIN" mo2kbc examples/hwmain_test.mo 2>&1)
if echo "$mnm_out" | grep -q "count rc=9" && echo "$mnm_out" | grep -q "ok    rc=1" \
   && echo "$mnm_out" | grep -q "sum   rc=-1518439201" && echo "$mnm_out" | grep -q "mode  rc=0" \
   && echo "$mnm_out" | grep -q "idx3  rc=495" && echo "$mnm_out" | grep -q "find  rc=1" \
   && echo "$mnm_out" | grep -q "probe rc=0" && echo "$mnm_out" | grep -q "idx6v rc=4093"; then
    green "  [PASS] hwmain .mo→kbc (hw_main()内置+双参imm动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwmain .mo→kbc"
    echo "$mnm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwmain mo2kbc")
fi

# =================  hw_wdbg 无线调试器信号层 (2026-09-29) =================
# 复刻立创开源「AI远程调试器」(ESP32-S3 无线串口调试器) 信号面:
#   串口桥接(2048B流缓冲/4档波特率/回环) + PWM测频占空比 + SPI捕获(50x64B)
#   + I2C读写捕获 + 自定义线序; 黄金值 = 默认线序表 FNV-1a-32 0xCD91F641
wdbg_st=$("$BIN" wdbg selftest 2>&1); wdbg_st_rc=$?
if [ $wdbg_st_rc -eq 0 ] && echo "$wdbg_st" | grep -q "all PASS" \
   && echo "$wdbg_st" | grep -q "golden=0xCD91F641"; then
    green "  [PASS] wdbg selftest (16组: 黄金/线序/波特率/桥接/监控/线序改复位/复位幂等)"
    PASS=$((PASS+1))
else
    red "  [FAIL] wdbg selftest (rc=$wdbg_st_rc)"
    echo "$wdbg_st" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("wdbg selftest")
fi

wdbg_card=$("$BIN" wdbg card 2>&1)
if echo "$wdbg_card" | grep -q "pin cksum = 0xCD91F641 (golden OK)" \
   && echo "$wdbg_card" | grep -q "bridge RD 4 字节: 4f 4b 0d 0a" \
   && echo "$wdbg_card" | grep -q "bridge RD 2 字节: 48 49" \
   && echo "$wdbg_card" | grep -q "pwm MEAS 1000Hz duty=250/1000" \
   && echo "$wdbg_card" | grep -q "spi XFER mode=3 rx: a4 a7 a6" \
   && echo "$wdbg_card" | grep -q "i2c RD addr=0x50 4 字节: 50 51 52 53"; then
    green "  [PASS] wdbg card (线序表/桥接回环/2048B流缓冲/PWM/SPI/I2C 全扇区)"
    PASS=$((PASS+1))
else
    red "  [FAIL] wdbg card (能力卡输出异常)"
    echo "$wdbg_card" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("wdbg card")
fi

# 一次性分发: 命中(0x00, exit 0) / 未识别(-1, exit 1)
wdbg1_out=$("$BIN" wdbg "bridge open 460800" 2>&1); wdbg1_rc=$?
wdbg2_out=$("$BIN" wdbg "bogus" 2>&1); wdbg2_rc=$?
if [ $wdbg1_rc -eq 0 ] && echo "$wdbg1_out" | grep -q "bridge OPEN @460800" \
   && [ $wdbg2_rc -eq 1 ] && echo "$wdbg2_out" | grep -q "NOCMD"; then
    green "  [PASS] wdbg one-shot (open命中rc=0/exit0, bogus未识别rc=-1/exit1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] wdbg one-shot (rc1=$wdbg1_rc rc2=$wdbg2_rc)"
    echo "$wdbg1_out" | sed 's/^/    /'; echo "$wdbg2_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("wdbg oneshot")
fi

# hw_wdbg .mo → kbc 端到端: hw_wdbg()内置 → OP_HW_WDBG_CALL → hw_wdbg_cmd
# 双参 imm 动态拼接判别性断言 (拼接失败即 rc 变 1/2):
#   hw_wdbg("pwm inject 1500 ", 300) → "pwm inject 1500 300" → 0
#   hw_wdbg("spi mode ", 2)          → "spi mode 2"          → 0
#   hw_wdbg("pin set uart 7 ", 8)    → "pin set uart 7 8"    → 0
wdm_out=$("$BIN" mo2kbc examples/wdbg_test.mo 2>&1)
if echo "$wdm_out" | grep -q "count rc=4" && echo "$wdm_out" | grep -q "mode  rc=0" \
   && echo "$wdm_out" | grep -q "nowr  rc=4" && echo "$wdm_out" | grep -q "badb  rc=2" \
   && echo "$wdm_out" | grep -q "open  rc=0" && echo "$wdm_out" | grep -q "rd4   rc=0" \
   && echo "$wdm_out" | grep -q "wr    rc=0" && echo "$wdm_out" | grep -q "rd2   rc=0" \
   && echo "$wdm_out" | grep -q "baud  rc=0" && echo "$wdm_out" | grep -q "close rc=0" \
   && echo "$wdm_out" | grep -q "pwm0  rc=4" && echo "$wdm_out" | grep -q "pwm1  rc=0" \
   && echo "$wdm_out" | grep -q "injv  rc=0" && echo "$wdm_out" | grep -q "spibad rc=2" \
   && echo "$wdm_out" | grep -q "xfer  rc=0" && echo "$wdm_out" | grep -q "spiv  rc=0" \
   && echo "$wdm_out" | grep -q "i2cbad rc=2" && echo "$wdm_out" | grep -q "pinbad rc=2" \
   && echo "$wdm_out" | grep -q "pinsetv rc=0" && echo "$wdm_out" | grep -q "pinrst rc=0" \
   && echo "$wdm_out" | grep -q "bogus rc=-1"; then
    green "  [PASS] wdbg .mo→kbc (hw_wdbg()内置+双参imm动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] wdbg .mo→kbc"
    echo "$wdm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("wdbg mo2kbc")
fi

# =====================================================================
# hw_flash — ESP32 ROM 下载协议烧录层 (2026-09-30)
# =====================================================================
fl_st=$("$BIN" flash selftest 2>&1); fl_st_rc=$?
if [ $fl_st_rc -eq 0 ] && echo "$fl_st" | grep -q "ALL PASS" \
   && echo "$fl_st" | grep -q "cmd table FNV-1a-32 == golden" \
   && echo "$fl_st" | grep -q "md5(4096B image) == golden" \
   && echo "$fl_st" | grep -q "1-bit wire corruption -> CHECKSUM"; then
    green "  [PASS] flash selftest (26 项: 黄金/命令表/SLIP/MD5/帧/协议/E2E/负向)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash selftest (rc=$fl_st_rc)"
    echo "$fl_st" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash selftest")
fi

fl_card=$("$BIN" flash card 2>&1)
if echo "$fl_card" | grep -q "黄金 FNV-1a-32 = 0xAF05978A" \
   && echo "$fl_card" | grep -q "0x13  FLASH_MD5" \
   && echo "$fl_card" | grep -q "0x14  SEC_INFO" \
   && echo "$fl_card" | grep -q "simulator (确定性 ROM)"; then
    green "  [PASS] flash card (命令表/黄金/块尺寸/传输源 全扇区)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash card (能力卡输出异常)"
    echo "$fl_card" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash card")
fi

# 端到端烧录 4096B → 设备 MD5 与本地逐位一致 (VERIFIED); bogus 未识别 exit1
fl1_out=$("$BIN" flash "run 4096" 2>&1); fl1_rc=$?
fl2_out=$("$BIN" flash bogus 2>&1); fl2_rc=$?
if [ $fl1_rc -eq 0 ] && echo "$fl1_out" | grep -q "4e328028738d17bb7ff82667d5803369  (VERIFIED)" \
   && echo "$fl1_out" | grep -q "4 blocks x 1024 B -> 100%" \
   && [ $fl2_rc -eq 1 ]; then
    green "  [PASS] flash one-shot (run 4096 命中 MD5 VERIFIED/exit0, bogus 未识别/exit1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash one-shot (rc1=$fl1_rc rc2=$fl2_rc)"
    echo "$fl1_out" | sed 's/^/    /'; echo "$fl2_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash oneshot")
fi

# CLI 空间隔参数 (2026-10-02 新增, 修复前静默跑错量级)
#   家族坑 #9「argv 下标」同源变体: hw_flash_cli 过去只取 argv[2],
#   `./xiaomo flash run 2048` 被截成 "run" → fl_cmd_run(0) → 悄悄烧 4096B,
#   输出还写"镜像 4096 B"无任何警告 —— 用户以为烧了 2048, 实际烧了 4096。
#   注意: .mo 侧 hw_flash("run ", 2048) 早就在测(见下方 mo2kbc 块),
#   唯独 CLI 的空格分隔写法没测 ⇒ 测试恰好绕开了这个 bug, 这本身就是教训。
# 判别法: 用非 4096 的尺寸(2048/1024), 若参数被吞则一律输出 4096。
flsp_out=$("$BIN" flash run 2048 2>&1); flsp_rc=$?
flbad_out=$("$BIN" flash run abc 2>&1); flbad_rc=$?
flbig_out=$("$BIN" flash run 99999 2>&1); flbig_rc=$?
if echo "$flsp_out" | grep -q "镜像 2048 B" && [ $flsp_rc -eq 0 ] \
   && echo "$flbad_out" | grep -q "bad size" && [ $flbad_rc -ne 0 ] \
   && echo "$flbig_out" | grep -q "model cap" && [ $flbig_rc -ne 0 ]; then
    green "  [PASS] flash CLI 空格参数 (run 2048 尺寸生效/错尺寸非0/超容量非0)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash CLI 空格参数 (rc=$flsp_rc/$flbad_rc/$flbig_rc)"
    echo "$flsp_out" | sed 's/^/    /'
    echo "$flbad_out" | sed 's/^/    /'
    echo "$flbig_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash cli 空格参数")
fi

# A-3 verify 假成功 (2026-10-02 新增)。修复前四重缺陷, 全部实测坐实:
#   ① fl_req(MD5, NULL, 0) 校验的是"空区间"          ② 从不比较 rbody 与本地 MD5
#   ③ return rc (协议层 OK 即 0) ⇒ 不匹配也"成功"    ④ hex 解码遇大写走错分支
#   修复前签名: `flash: verify -> rc=0 md5_match=0` 且 **退出码 0** —— 自相矛盾。
#
# ⚠️ 判别力的来源是"**签名 + 退出码**"两条同时成立:
#   只断言"rc != 0"太弱(任何失败都满足); 只断言输出太弱(措辞会变)。
#   `md5_match=` 是修复前独有的陈旧打印, rc=0 是修复前独有的假成功 —— 两条合起来
#   才把"旧行为"和"新行为"切开。注意本地 rc 用 $(...) 捕获, 绝不过管道
#   (管道末端 sed 的码会覆盖真码, 这是本项目第三次踩)。
#
# 🕳️ 局限声明(必须诚实): 模拟器下 verify 永远诚实失败(rc=4), 因为设备模型
#   g_model 是**进程内 static**, 而 run/verify 是两个独立进程 ⇒ 跨进程必读空。
#   所以本测试锁的是"**不许假成功**", 不是"能 MATCH"。真机 MATCH 需另安排。
#   连带锁住的还有越界读: 修复前 verify 传 addr=0x10000 而 g_model 仅 32768B
#   ⇒ 越界读 32KB 之外的相邻内存, 会吐出一个"随机但看起来合法"的 MD5。
flv_out=$("$BIN" flash verify 2048 2>&1); flv_rc=$?
flvbad_out=$("$BIN" flash verify abc 2>&1); flvbad_rc=$?
flvbig_out=$("$BIN" flash verify 99999 2>&1); flvbig_rc=$?
flvzero_out=$("$BIN" flash verify 0 2>&1); flvzero_rc=$?
# verify 0 必须被拒(不猜量级, A-0 教训) ; 99999 报 model cap ; abc 报 bad size
if [ $flv_rc -ne 0 ] && ! echo "$flv_out" | grep -q "md5_match=" \
   && [ $flvbad_rc -eq 2 ] && echo "$flvbad_out" | grep -q "bad size" \
   && [ $flvbig_rc -eq 6 ] && echo "$flvbig_out" | grep -q "model cap" \
   && [ $flvzero_rc -ne 0 ] && echo "$flvzero_out" | grep -q "bad size"; then
    green "  [PASS] flash verify 无假成功 (rc≠0/无陈旧 md5_match/错参非0/超容量6/零值拒)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash verify 无假成功 (rc=$flv_rc/$flvbad_rc/$flvbig_rc/$flvzero_rc)"
    echo "$flv_out" | sed 's/^/    /'
    echo "$flvbad_out" | sed 's/^/    /'
    echo "$flvbig_out" | sed 's/^/    /'
    echo "$flvzero_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash verify 无假成功")
fi

# hw_flash .mo → kbc 端到端: hw_flash()内置 → OP_HW_FLASH_CALL → hw_flash_cmd
# 双参 imm 动态拼接判别性 + 越界/未识别/help 返回码全覆盖:
#   hw_flash("run ", 2048) → "run 2048" → 0 ; hw_flash("run 99999") → 6 (BADSIZE)
flm_out=$("$BIN" mo2kbc examples/flash_test.mo 2>&1)
if echo "$flm_out" | grep -q "mode  rc=0" && echo "$flm_out" | grep -q "sync  rc=0" \
   && echo "$flm_out" | grep -q "chip  rc=0" && echo "$flm_out" | grep -q "run4k rc=0" \
   && echo "$flm_out" | grep -q "run2k rc=0" && echo "$flm_out" | grep -q "runsz rc=6" \
   && echo "$flm_out" | grep -q "md5   rc=0" && echo "$flm_out" | grep -q "slip  rc=0" \
   && echo "$flm_out" | grep -q "stat  rc=0" && echo "$flm_out" | grep -q "help  rc=-2" \
   && echo "$flm_out" | grep -q "bogus rc=-1" \
   && echo "$flm_out" | grep -q "0cc61b9a600f965b827d59f65cbf9e35  (VERIFIED)"; then
    green "  [PASS] flash .mo→kbc (hw_flash()内置+双参 imm 动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] flash .mo→kbc"
    echo "$flm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("flash mo2kbc")
fi

# =====================================================================
# hw_pin — 引脚档案/双模驱动/编程电压层 (2026-09-30)
# =====================================================================
pn_st=$("$BIN" pin selftest 2>&1); pn_st_rc=$?
if [ $pn_st_rc -eq 0 ] \
   && echo "$pn_st" | grep -q "T2 golden table OK" \
   && echo "$pn_st" | grep -q "T5 bit-bang RDID = EF4018 OK" \
   && echo "$pn_st" | grep -q "T7 page program+verify OK" \
   && echo "$pn_st" | grep -q "T8 write-protect negative (no WREN) OK" \
   && echo "$pn_st" | grep -q "T12 unmapped-pin reject OK" \
   && echo "$pn_st" | grep -q "T13 ISP cmdsum table OK" \
   && echo "$pn_st" | grep -q "T14 ISP handshake + GetID(0x0410) OK" \
   && echo "$pn_st" | grep -q "T15 ISP mirror (erase->write256->read) OK" \
   && echo "$pn_st" | grep -q "T16 ISP no-uart profile reject OK" \
   && echo "$pn_st" | grep -q "T17 ISP bad-checksum -> NACK OK"; then
    green "  [PASS] pin selftest (17 项: 黄金/档案/映射/RDID/擦除/编程/写保护负向/电压/无效档案/未映射 + ISP 5 项)"
    PASS=$((PASS+1))
else
    red "  [FAIL] pin selftest (rc=$pn_st_rc)"
    echo "$pn_st" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("pin selftest")
fi

pn_card=$("$BIN" pin card 2>&1)
if echo "$pn_card" | grep -q "golden=0x9E0F10FA" \
   && echo "$pn_card" | grep -q "profiles=4" \
   && echo "$pn_card" | grep -q "MOSI=IO23 MISO=IO19 CK=IO18 CS=IO5" \
   && echo "$pn_card" | grep -q "isp(AN3155 UART cmdsum=0xD2A9A924)" \
   && echo "$pn_card" | grep -q "drv=BB"; then
    green "  [PASS] pin card (档案表/黄金/信号→GPIO 映射 全扇区)"
    PASS=$((PASS+1))
else
    red "  [FAIL] pin card (能力卡输出异常)"
    echo "$pn_card" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("pin card")
fi

# 一次性: wtest 全链 OK/exit0, id 读到 EF4018/exit0, bogus 未识别/exit1
pn_w=$("$BIN" pin wtest 2>&1); pn_w_rc=$?
pn_b=$("$BIN" pin bogus 2>&1); pn_b_rc=$?
pn_id=$("$BIN" pin id 2>&1); pn_id_rc=$?
if [ $pn_w_rc -eq 0 ] && echo "$pn_w" | grep -q "wtest(256B) -> OK (0)" \
   && [ $pn_b_rc -eq 1 ] \
   && [ $pn_id_rc -eq 0 ] && echo "$pn_id" | grep -q "EF 40 18"; then
    green "  [PASS] pin one-shot (wtest OK/exit0, id=EF4018/exit0, bogus 未识别/exit1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] pin one-shot (rc w=$pn_w_rc b=$pn_b_rc id=$pn_id_rc)"
    echo "$pn_w" | sed 's/^/    /'; echo "$pn_id" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("pin oneshot")
fi

# pin isp 一次性: 命令表黄金/握手对拍/烧录全链/负向 (L3 插件 #2 AN3155)
pn_isp_chk=$("$BIN" pin isp chk 2>&1); pn_isp_chk_rc=$?
pn_isp_inf=$("$BIN" pin isp info 2>&1); pn_isp_inf_rc=$?
pn_isp_wt=$("$BIN" pin isp wtest 2>&1); pn_isp_wt_rc=$?
if [ $pn_isp_chk_rc -eq 0 ] && echo "$pn_isp_chk" | grep -q "cmdsum=0xD2A9A924 golden=0xD2A9A924 -> OK" \
   && [ $pn_isp_inf_rc -eq 0 ] \
   && echo "$pn_isp_inf" | grep -q "ISP ver=0x31 PID=0x0410 cmdsum=0xD2A9A924 (golden=0xD2A9A924)" \
   && [ $pn_isp_wt_rc -eq 0 ] \
   && echo "$pn_isp_wt" | grep -q "ISP wtest(256B @0x08000000) -> OK (0)"; then
    green "  [PASS] pin isp one-shot (AN3155: 握手/GetID 0x0410/命令表黄金/擦写读全链)"
    PASS=$((PASS+1))
else
    red "  [FAIL] pin isp one-shot (rc chk=$pn_isp_chk_rc inf=$pn_isp_inf_rc wt=$pn_isp_wt_rc)"
    echo "$pn_isp_inf" | sed 's/^/    /'; echo "$pn_isp_wt" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("pin isp oneshot")
fi

# hw_pin .mo → kbc 端到端: hw_pin()内置 → OP_HW_PIN_CALL → hw_pin_cmd
# 双参 imm 动态拼接判别性: hw_pin("vpp ", 3) → "vpp 3" → 12500 mV (拼接失败则落 0 档)
pnm_out=$("$BIN" mo2kbc examples/pin_test.mo 2>&1)
if echo "$pnm_out" | grep -q "mode   rc=0" && echo "$pnm_out" | grep -q "profs  rc=0" \
   && echo "$pnm_out" | grep -q "load   rc=0" && echo "$pnm_out" | grep -q "id     rc=0" \
   && echo "$pnm_out" | grep -q "wtest  rc=0" && echo "$pnm_out" | grep -q "vpp2   rc=0" \
   && echo "$pnm_out" | grep -q "vpprd  rc=0" && echo "$pnm_out" | grep -q "vpp3   rc=0" \
   && echo "$pnm_out" | grep -q "erase  rc=0" && echo "$pnm_out" | grep -q "stat   rc=0" \
   && echo "$pnm_out" | grep -q "help   rc=-2" && echo "$pnm_out" | grep -q "bogus  rc=-1" \
   && echo "$pnm_out" | grep -q "VPP set 3 (12500 mV)" \
   && echo "$pnm_out" | grep -q "RDID -> rc=0 jedec=0x1840EF (EF 40 18)" \
   && echo "$pnm_out" | grep -q "ispchk rc=0" && echo "$pnm_out" | grep -q "ispinf rc=0" \
   && echo "$pnm_out" | grep -q "ispwrt rc=0" \
   && echo "$pnm_out" | grep -q "ISP ver=0x31 PID=0x0410" \
   && echo "$pnm_out" | grep -q "ISP wtest(256B @0x08000000) -> OK (0)"; then
    green "  [PASS] pin .mo→kbc (hw_pin()内置+双参 imm 动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] pin .mo→kbc"
    echo "$pnm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("pin mo2kbc")
fi

# ---- hw_dc DC 电源信号层 (2026-10-01) ----
dc_out=$("$BIN" dc 2>&1); dc_rc=$?
if [ $dc_rc -eq 0 ] && echo "$dc_out" | grep -q "golden OK" \
   && echo "$dc_out" | grep -q "count=8" \
   && echo "$dc_out" | grep -q "base IN =0x059  base OUT=0x080" \
   && echo "$dc_out" | grep -q "DATA_5 = 0x0D9"; then
    green "  [PASS] hwdc (能力卡/黄金校验和/信号表/参考预值/组合数据帧)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc (能力卡输出异常 rc=$dc_rc)"
    echo "$dc_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc")
fi

# hwdc 自检 (黄金锁定/信号表/极值窗口/槽位API/BSP注入/命令分发)
dcs_out=$("$BIN" dc selftest 2>&1); dcs_rc=$?
if [ $dcs_rc -eq 0 ] && echo "$dcs_out" | grep -q "all PASS"; then
    green "  [PASS] hwdc selftest (黄金锁定/信号表/极值窗口/槽位API/BSP注入/命令分发)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc selftest (rc=$dcs_rc)"
    echo "$dcs_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc selftest")
fi

# hw_dc .mo → kbc 端到端: hw_dc()内置 → OP_HW_DC_CALL → hw_dc_cmd
# 双参 imm 动态拼接判别性: hw_dc("data ", 4) → "data 4" → 217 (失败则 0);
#   hw_dc("base ", 1) → "base 1" → 128 (失败则 89)
dcm_out=$("$BIN" mo2kbc examples/dc_test.mo 2>&1)
if echo "$dcm_out" | grep -q "count  rc=8" && echo "$dcm_out" | grep -q "dcount rc=7" \
   && echo "$dcm_out" | grep -q "ok     rc=1" \
   && echo "$dcm_out" | grep -q "baseIN rc=89" && echo "$dcm_out" | grep -q "baseOT rc=128" \
   && echo "$dcm_out" | grep -q "data4  rc=217" && echo "$dcm_out" | grep -q "sig0   rc=42" \
   && echo "$dcm_out" | grep -q "range0 rc=0" \
   && echo "$dcm_out" | grep -q "dyn4   rc=217" && echo "$dcm_out" | grep -q "dynB   rc=128" \
   && echo "$dcm_out" | grep -q "bogus  rc=-1" && echo "$dcm_out" | grep -q "help   rc=-2"; then
    green "  [PASS] hwdc .mo→kbc (hw_dc()内置+双参 imm 动态拼接 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc .mo→kbc"
    echo "$dcm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc mo2kbc")
fi

# ---- hw_dc DCPP 供电协议 (帧层, 2026-10-01) ----
# 协议黄金 0x9432D6A5 (独立 Python 对拍) / 9 命令 / START(80 EF 02)..END(ED FF 0D)
dcp_out=$("$BIN" dc proto card 2>&1); dcp_rc=$?
if [ $dcp_rc -eq 0 ] && echo "$dcp_out" | grep -q "cksum= 0x9432D6A5 (golden OK)" \
   && echo "$dcp_out" | grep -q "cmds=9" \
   && echo "$dcp_out" | grep -q "count=9" \
   && echo "$dcp_out" | grep -q "PING   frame=11" \
   && echo "$dcp_out" | grep -q "hex=80EF020100010010EDFF0D"; then
    green "  [PASS] hwdc proto card (协议黄金/9命令/帧定界/打帧字节级)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc proto card (rc=$dcp_rc)"
    echo "$dcp_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc proto card")
fi

# 会话门禁 + 一条龙: 复位→OPEN→会话内含 6 命令→CLOSE; 9 帧全成帧 err=0
dcpl_out=$("$BIN" dc proto loop 2>&1); dcpl_rc=$?
if [ $dcpl_rc -eq 0 ] && echo "$dcpl_out" | grep -q "ALL FRAMED  rx=9 tx=9 err=0 last=CLOSE"; then
    green "  [PASS] hwdc proto loop (会话门禁: 复位/OPEN/会话内命令/CLOSE 一条龙 9帧 err=0)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc proto loop (rc=$dcpl_rc)"
    echo "$dcpl_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc proto loop")
fi

# 单命令: run 8 (OPEN) 命中 成帧 rc=1 / opened=1
dcpr_out=$("$BIN" dc proto run 8 2>&1); dcpr_rc=$?
if [ $dcpr_rc -eq 0 ] && echo "$dcpr_out" | grep -q "run OPEN  rc=1  opened=1"; then
    green "  [PASS] hwdc proto run (单命令 OPEN 成帧 rc=1/opened=1)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc proto run (rc=$dcpr_rc)"
    echo "$dcpr_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc proto run")
fi

# hw_dc DCPP .mo → kbc 端到端: hw_dc("proto*")内置 → OP_HW_DC_CALL → hw_dc_cmd
# 判别性: 帧长公式(0→11/16→27/17→-1) + 会话门禁(GETx=-7) + OPEN/CLOSE opened 翻转
#         + 应答值(DATA→217 / PING→80) + 双参 imm 动态拼接(protobuild 1→11)
dcpm_out=$("$BIN" mo2kbc examples/dc_proto_test.mo 2>&1)
if echo "$dcpm_out" | grep -q "ver    rc=1" && echo "$dcpm_out" | grep -q "count  rc=9" \
   && echo "$dcpm_out" | grep -q "ok     rc=1" \
   && echo "$dcpm_out" | grep -q "fr0    rc=11" && echo "$dcpm_out" | grep -q "fr16   rc=27" \
   && echo "$dcpm_out" | grep -q "fr17   rc=-1" \
   && echo "$dcpm_out" | grep -q "bPING  rc=11" && echo "$dcpm_out" | grep -q "bGET   rc=12" \
   && echo "$dcpm_out" | grep -q "GETx   rc=-7" \
   && echo "$dcpm_out" | grep -q "OPEN   rc=1" && echo "$dcpm_out" | grep -q "opened rc=1" \
   && echo "$dcpm_out" | grep -q "value  rc=217" && echo "$dcpm_out" | grep -q "pong   rc=80" \
   && echo "$dcpm_out" | grep -q "CLOSE  rc=1" && echo "$dcpm_out" | grep -q "opened rc=0" \
   && echo "$dcpm_out" | grep -q "GETx2  rc=-7" && echo "$dcpm_out" | grep -q "dynB   rc=11"; then
    green "  [PASS] hwdc proto .mo→kbc (帧层协议+会话门禁+应答值+双参 imm 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] hwdc proto .mo→kbc"
    echo "$dcpm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("hwdc proto mo2kbc")
fi

# ---- hw_dmc DMC 主从链路协议层 (第十一位 hw 家族成员, 2026-10-01) ----
# 黄金 0x169A603E (独立 Python 对拍) / 9 命令 / 7 状态 / 8 错误码
# 帧 = SYNC0|SYNC1|LEN|CMD|PAYLOAD|CRC_LO|CRC_HI, CRC16-CCITT 统一 (原三份文件两份互不兼容)
# 三份原始资料"一字不改"归档在 src/hw/hw_dmc_base.c, 由 tools/dmc_verbatim_check.py 反解校验
dmc_out=$("$BIN" dmc selftest 2>&1); dmc_rc=$?
if [ $dmc_rc -eq 0 ] && echo "$dmc_out" | grep -q "DMC selftest: ALL PASS (fails=0)"; then
    green "  [PASS] dmc selftest (26 项: 黄金/CRC向量/帧往返/先校验后索引/上界/保BSP/握手/半双工/双参拼接/草稿API·NACK·长度出参)"
    PASS=$((PASS+1))
else
    red "  [FAIL] dmc selftest (rc=$dmc_rc)"
    echo "$dmc_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc selftest")
fi

# 能力卡: 命令表 / 状态表 / 黄金 / 帧布局 / CRC 标准向量 / HELLO 打包往返
dmcc_out=$("$BIN" dmc cmds 2>&1); dmcs_out=$("$BIN" dmc states 2>&1)
dmcg_out=$("$BIN" dmc golden 2>&1); dmcf_out=$("$BIN" dmc frame 2>&1)
dmcv_out=$("$BIN" dmc crcvec 2>&1); dmch_out=$("$BIN" dmc hello 2>&1)
if echo "$dmcc_out" | grep -q "01 HELLO" && echo "$dmcc_out" | grep -q "0F NACK" \
   && [ "$(echo "$dmcc_out" | grep -c '^  0')" -eq 9 ] \
   && echo "$dmcs_out" | grep -q "5 ESTABLISHED" && echo "$dmcs_out" | grep -q "6 ERROR" \
   && [ "$(echo "$dmcs_out" | grep -c '^  [0-6] ')" -eq 7 ] \
   && echo "$dmcg_out" | grep -q "golden=0x169A603E expect=0x169A603E OK" \
   && echo "$dmcf_out" | grep -q "min=6 max=255 max_payload=249" \
   && echo "$dmcf_out" | grep -q "实测: HELLO(8B payload) pack = 14 B" \
   && echo "$dmcv_out" | grep -q 'crc("123456789")=0x29B1 expect=0x29B1' \
   && echo "$dmch_out" | grep -q "pack len=14 unpack rc=0 cmd=HELLO plen=8 match=1"; then
    green "  [PASS] dmc card (命令表9/状态表7/黄金/帧布局/CRC标准向量0x29B1/HELLO打包往返)"
    PASS=$((PASS+1))
else
    red "  [FAIL] dmc card"
    for o in "$dmcc_out" "$dmcs_out" "$dmcg_out" "$dmcf_out" "$dmcv_out" "$dmch_out"; do echo "$o" | sed 's/^/    /'; done
    FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc card")
fi

# one-shot: 合法命中 rc=0/exit0; 未识别命令本地就拒 rc=255/exit1 (不上网=不会变成"发了个别的")
"$BIN" dmc count >/dev/null 2>&1; dmc_ok=$?
"$BIN" dmc boguszzz >/dev/null 2>&1; dmc_bad=$?
if [ $dmc_ok -eq 0 ] && [ $dmc_bad -eq 255 ]; then
    green "  [PASS] dmc one-shot (count 命中 exit0, bogus 未识别 exit255)"
    PASS=$((PASS+1))
else
    red "  [FAIL] dmc one-shot (ok=$dmc_ok bad=$dmc_bad)"
    FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc one-shot")
fi

# 归档完整性: 三份原始资料"一字不改" (按哨兵反解 → 与 tests/dmc_orig 逐字节 diff)
if command -v python3 >/dev/null 2>&1; then
    vbc_out=$(python3 tools/dmc_verbatim_check.py 2>&1)
    if echo "$vbc_out" | grep -q "VERBATIM OK (3/3 一字未改)"; then
        green "  [PASS] dmc verbatim (三份原始资料一字未改: 伪代码底稿/handshake/porto)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] dmc verbatim"
        echo "$vbc_out" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc verbatim")
    fi
else
    echo "  [SKIP] dmc verbatim (缺 python3)"
fi

# hw_dmc .mo → kbc 端到端: hw_dmc()内置 → OP_HW_DMC_CALL → hw_dmc_cmd
# 判别性: 单参常量(count/golden/状态/错误码) + 双参 imm 动态拼接(add/sub/mul 真参与运算
#         → 109/91/498, 证明数值不是被静默忽略) + 负向诚实失败(overflow/badpfx=-1, help=-2, zzz=-1)
dmcm_out=$("$BIN" mo2kbc examples/dmc_test.mo 2>&1)
if echo "$dmcm_out" | grep -q "count    rc=9" && echo "$dmcm_out" | grep -q "golden   rc=379215934" \
   && echo "$dmcm_out" | grep -q "ok       rc=1" && echo "$dmcm_out" | grep -q "states   rc=7" \
   && echo "$dmcm_out" | grep -q "errs     rc=8" && echo "$dmcm_out" | grep -q "maxpay   rc=249" \
   && echo "$dmcm_out" | grep -q "maxframe rc=255" && echo "$dmcm_out" | grep -q "crcvec   rc=1" \
   && echo "$dmcm_out" | grep -q "hello    rc=1" \
   && echo "$dmcm_out" | grep -q "add100  rc=109" && echo "$dmcm_out" | grep -q "sub100  rc=91" \
   && echo "$dmcm_out" | grep -q "mul2    rc=498" && echo "$dmcm_out" | grep -q "num7    rc=7" \
   && echo "$dmcm_out" | grep -q "overflow rc=-1" && echo "$dmcm_out" | grep -q "badpfx  rc=-1" \
   && echo "$dmcm_out" | grep -q "help     rc=-2" && echo "$dmcm_out" | grep -q "zzz      rc=-1"; then
    green "  [PASS] dmc .mo→kbc (单参+双参imm动态拼接+负向诚实失败 六层全链路)"
    PASS=$((PASS+1))
else
    red "  [FAIL] dmc .mo→kbc"
    echo "$dmcm_out" | sed 's/^/    /'
    FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc mo2kbc")
fi

# 全跨矩阵 + ⭐黄金锁 (2026-10-01 接入回归网)
#   为什么必须自动跑: 矩阵原是孤儿脚本(全库无调用), 六模式指纹全靠人肉记得手动跑。
#   而它只验 C11==C++17 自一致, 从不与记录比对 ⇒ 源码一改, 知识页指纹静默腐烂
#   (本页已作废三次)。加第 5 项黄金锁后, 腐烂成为 FAIL; 但若不自动跑, 依旧靠人记得。
#   代价: 约 19s (本回归 4.2s → 约 24s)。可用 XIAOMO_SKIP_MATRIX=1 关闭。
if [ "${XIAOMO_SKIP_MATRIX:-0}" = "1" ]; then
    echo "  [SKIP] dmc matrix+golden (XIAOMO_SKIP_MATRIX=1)"
elif ! command -v cc >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
    echo "  [SKIP] dmc matrix+golden (缺 cc/c++)"
else
    dmcmx_out=$(bash tools/dmc_matrix.sh 2>&1); dmcmx_rc=$?
    # 不写死 PASS=N (加检查项就会自伤), 只判: 退出码 0 且 FAIL 数为 0 且黄金锁通过
    dmcmx_fail=$(echo "$dmcmx_out" | sed -n 's/.*全跨矩阵: PASS=[0-9]* FAIL=\([0-9]*\).*/\1/p' | tail -1)
    if [ $dmcmx_rc -eq 0 ] && [ "${dmcmx_fail:-1}" = "0" ] \
       && echo "$dmcmx_out" | grep -q "7 项指纹与记录逐位一致"; then
        green "  [PASS] dmc matrix+golden (全跨 16 项: C11==C++17/六模式 override/双交叉 freestanding/⭐黄金锁 7 指纹未漂移)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] dmc matrix+golden (rc=$dmcmx_rc 矩阵FAIL数=${dmcmx_fail:-?})"
        echo "$dmcmx_out" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("dmc matrix+golden")
    fi
fi

# hw_wdbg 全跨矩阵 (2026-10-02 接入)
#   为什么必须有: wdbg 一直只靠本回归里的宿主 C++17 一条腿, 从未在 C11 形态下
#   编译过。本轮把 g_bsp 由「指针」改成「结构体 + g_bsp_set」= 数据结构改动,
#   而数据结构改动正是 C 形态/交叉编译的风险区 (锚点: 自证工具与被测物共享盲点)。
#   同样不能是孤儿脚本 —— dmc 那条注释已记过这个坑, 不重复犯。
if [ "${XIAOMO_SKIP_MATRIX:-0}" = "1" ]; then
    echo "  [SKIP] wdbg matrix (XIAOMO_SKIP_MATRIX=1)"
elif ! command -v cc >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
    echo "  [SKIP] wdbg matrix (缺 cc/c++)"
else
    wdmx_out=$(bash tools/wdbg_matrix.sh 2>&1); wdmx_rc=$?
    # 同样不写死 PASS=N, 只判退出码 (脚本以 FAIL 数作退出码)
    if [ $wdmx_rc -eq 0 ]; then
        green "  [PASS] wdbg matrix (全跨 15 项: C11==C++17 逐位一致/六模式 override+⭐六模式行为对拍/双交叉 freestanding)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] wdbg matrix (rc=$wdmx_rc)"
        echo "$wdmx_out" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("wdbg matrix")
    fi
fi

# hw_main 全跨矩阵 (2026-10-03 接入)
#   为什么必须有: 与 dmc/wdbg 同源, 但多一个此前没人碰过的维度 ——
#   前面两者的 g_bsp 都是【指针】, hw_main 的 g_bsp 是【结构体 + g_bsp_set】,
#   且带 hw_fault.hw_set/fw_set, 属数据结构改动 = C 形态/交叉编译的风险区。
#   同样不能是孤儿脚本: 上面 dmc 那条注释已记过"全库无调用"的坑, 不重复犯。
#   注意 macOS 弱语义: 桩必须带 weak_import, 否则 C++ 侧与 C 侧不共用同一符号。
if [ "${XIAOMO_SKIP_MATRIX:-0}" = "1" ]; then
    echo "  [SKIP] hwmain matrix (XIAOMO_SKIP_MATRIX=1)"
elif ! command -v cc >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
    echo "  [SKIP] hwmain matrix (缺 cc/c++)"
else
    hnmx_out=$(bash tools/hwmain_matrix.sh 2>&1); hnmx_rc=$?
    if [ $hnmx_rc -eq 0 ]; then
        green "  [PASS] hwmain matrix (全跨 16 项: C11==C++17/六模式 override 零告警/双交叉 freestanding/9 探针/双向对拍)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] hwmain matrix (rc=$hnmx_rc)"
        echo "$hnmx_out" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("hwmain matrix")
    fi
fi

# hw_main 判定器变异 (2026-10-03 接入) —— 验证【裁判】而非【产品】
#   这一条与上面所有测试都不同: 其余验的是"代码对不对", 这一条验的是
#   "我们的检查是不是真的在检查"。全绿的测试不证明测试有效。
#   5 条变异: MV1 桩吐错值 / MV2 摘 extern "C" / MV3 共用代码差异(应存活) /
#             MV4 形态专属差异 / MV5 摘掉汇总累加。
#   ⚠️ 代价高: 实测 87s (要连跑 7 遍全矩阵, 单遍矩阵 16s), 故默认关闭,
#      用 XIAOMO_SKIP_VARIANT=0 显式打开。默认回归因此从 33s -> 49s (多挂 matrix),
#      打开变异则再涨到约 136s。
#   为什么默认关闭而不是默认打开: 有人每天跑 make test, 每天 +87s 会变成每天都付的税,
#      而一旦大家习惯性地跳过它, 它就退化成孤儿脚本 —— 与默认打开无异。
#   为什么仍然要挂在网里: 孤儿脚本 = 靠人记得, 而"人记得"已经失败过三次。
#      挂进网 + 默认跳过, 至少让人【看得见它存在且知道怎么开】。
#   纪律: 判据有两条命 —— 验产品的那条(上面)和验裁判的这条(这里)。
if [ "${XIAOMO_SKIP_VARIANT:-1}" = "1" ]; then
    echo "  [SKIP] hwmain variant (变异验裁判; 开: XIAOMO_SKIP_VARIANT=0)"
elif ! command -v cc >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
    echo "  [SKIP] hwmain variant (缺 cc/c++)"
else
    echo "  ... hw_main 判定器变异 (5 条, 耗时较长, 耐心等)"
    hnvar_out=$(bash tools/hwmain_matrix_mutate.sh 2>&1); hnvar_rc=$?
    if [ $hnvar_rc -eq 0 ]; then
        green "  [PASS] hwmain variant (5 条变异全部符合预期: 4 杀 1 存活, 裁判确实在检查)"
        PASS=$((PASS+1))
    else
        red "  [FAIL] hwmain variant (rc=$hnvar_rc) —— 判据本身失效, 上面所有绿灯都不作数"
        echo "$hnvar_out" | sed 's/^/    /'
        FAIL=$((FAIL+1)); FAILED_NAMES+=("hwmain variant")
    fi
fi


echo "================  汇总 ================"
echo "  通过: $PASS   失败: $FAIL"
if [ $FAIL -gt 0 ]; then
    red "  失败项: ${FAILED_NAMES[*]}"
    exit 1
else
    green "  全部通过 ✔"
    exit 0
fi
