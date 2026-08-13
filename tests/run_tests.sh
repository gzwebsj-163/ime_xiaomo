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
echo "================  汇总 ================"
echo "  通过: $PASS   失败: $FAIL"
if [ $FAIL -gt 0 ]; then
    red "  失败项: ${FAILED_NAMES[*]}"
    exit 1
else
    green "  全部通过 ✔"
    exit 0
fi
