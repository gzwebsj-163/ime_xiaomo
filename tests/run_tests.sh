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
    case "$name" in
      mlp*|train_*|nd_tensor_test|linux_boot|openclaw_interact|hwdev_test|hwfault_test)
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

echo ""
echo "================  汇总 ================"
echo "  通过: $PASS   失败: $FAIL"
if [ $FAIL -gt 0 ]; then
    red "  失败项: ${FAILED_NAMES[*]}"
    exit 1
else
    green "  全部通过 ✔"
    exit 0
fi
