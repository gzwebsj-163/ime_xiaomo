#!/usr/bin/env bash
# ============================================================================
# hw_pwm 全跨式验证矩阵 (不采信自己, 每项独立口径)
#
#   1. C11 vs C++17 (默认/HOST 模式) : 同源编译 → 跑同一组确定性子命令 → 输出 md5 逐位一致
#   2. 六模式 override 编译零警告     : -DHW_PWM_MODE_OVERRIDE=0..5 各自独立编译
#   3. freestanding 双交叉            : riscv32-esp-elf / xtensa-esp-elf 加 -ffreestanding
#   4. ⭐ 六模式逐一 C11 vs C++17 行为对拍 —— 补"只验能编译"的缺口
#   5. ⭐⭐ 黄金锁 (实测 vs 记录在案) —— 补"自一致 != 与记录一致"的缺口
#   6. ⭐⭐⭐ 独立 Python 黄金对拍 —— 补"C 侧期望与实现同源"的缺口
#
# 位置说明: 本脚本曾寄居 tmp/, 那是易清理区, 验证工具寄居临时目录 = 下次复现时
#           它可能已经不在了(与 tools/dmc_matrix.sh 同款教训)。故置于 tools/。
# ============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1
OUT=tmp/pwm_matrix
GOLDEN="$HERE/pwm_golden.txt"
UPDATE_GOLDEN=0
[ "${1:-}" = "--update-golden" ] && UPDATE_GOLDEN=1
mkdir -p "$OUT"
SRCS="src/hw/hw_pwm.c tests/pwm_host_main.c"
INC="-Iinclude -Isrc"
PASS=0; FAIL=0
H_MD5=""; MODE_MD5=()

# 固定子命令序列 —— 全部只读/纯算术, 无时间量、无随机量、无硬件依赖。
# ⚠️ 不得加入会改动全局状态的命令(init/start/stop/ch): 顺序一改,
#    后面的 get 就会读到前一条留下的状态, md5 漂移却查不出是谁的锅。
CMDS=(cmd:mode cmd:golden cmd:regcnt cmd:pincnt cmd:bsp
      cmd:reg0 cmd:reg3 cmd:reg6 cmd:reg7 cmd:regidx4096
      cmd:duty0 cmd:duty50 cmd:duty100 cmd:duty101
      cmd:get0 cmd:start0 cmd:stop0 cmd:oep cmd:bogus)

run_cmds() {   # $1=binary  $2=outfile
    : > "$2"
    local c
    for c in "${CMDS[@]}"; do
        printf '## %s\n' "$c" >> "$2"
        case "$c" in
            cmd:reg0)       "$1" cmd reg 0        >> "$2" 2>&1 ;;
            cmd:reg3)       "$1" cmd reg 3        >> "$2" 2>&1 ;;
            cmd:reg6)       "$1" cmd reg 6        >> "$2" 2>&1 ;;
            cmd:reg7)       "$1" cmd reg 7        >> "$2" 2>&1 ;;
            cmd:regidx4096) "$1" cmd regidx 4096  >> "$2" 2>&1 ;;
            cmd:duty0)      "$1" cmd duty 0       >> "$2" 2>&1 ;;
            cmd:duty50)     "$1" cmd duty 50      >> "$2" 2>&1 ;;
            cmd:duty100)    "$1" cmd duty 100     >> "$2" 2>&1 ;;
            cmd:duty101)    "$1" cmd duty 101     >> "$2" 2>&1 ;;
            cmd:get0)       "$1" cmd get 0        >> "$2" 2>&1 ;;
            cmd:start0)     "$1" cmd start 0      >> "$2" 2>&1 ;;
            cmd:stop0)      "$1" cmd stop 0       >> "$2" 2>&1 ;;
            cmd:*)          "$1" cmd "${c#cmd:}" >> "$2" 2>&1 ;;
        esac
    done
}

echo "================ 1. C11 vs C++17 输出 md5 逐位对拍 (默认/HOST 模式) ================"
cc  -std=c11   -Wall -Wextra $INC -o "$OUT/pwm_c11"   $SRCS 2>"$OUT/c11.log"
r1=$?
c++ -std=c++17 -Wall -Wextra $INC -x c++ -o "$OUT/pwm_cpp17" $SRCS 2>"$OUT/cpp17.log"
r2=$?
warn1=$(grep -c -i "warning" "$OUT/c11.log")
warn2=$(grep -c -i "warning" "$OUT/cpp17.log")
echo "  C11  build rc=$r1 warnings=$warn1"
echo "  C++17 build rc=$r2 warnings=$warn2"
if [ $r1 -ne 0 ] || [ $r2 -ne 0 ]; then
    echo "  [FAIL] 编译失败"; sed 's/^/    /' "$OUT/c11.log" "$OUT/cpp17.log"; FAIL=$((FAIL+1))
else
    run_cmds "$OUT/pwm_c11"   "$OUT/c11.txt"
    run_cmds "$OUT/pwm_cpp17" "$OUT/cpp17.txt"
    m1=$(md5 -q "$OUT/c11.txt"); m2=$(md5 -q "$OUT/cpp17.txt")
    H_MD5="$m1"
    echo "  C11   out md5=$m1"
    echo "  C++17 out md5=$m2"
    if [ "$m1" = "$m2" ] && [ "$warn1" -eq 0 ] && [ "$warn2" -eq 0 ]; then
        echo "  [PASS] C11 == C++17 逐位一致, 两端零警告"; PASS=$((PASS+1))
    else
        echo "  [FAIL] 输出不一致或存在警告"; diff "$OUT/c11.txt" "$OUT/cpp17.txt" | head -20
        FAIL=$((FAIL+1))
    fi
fi

echo "================ 2. 六模式 override 编译零警告 ================"
for m in 0 1 2 3 4 5; do
    cc -std=c11 -Wall -Wextra $INC -DHW_PWM_MODE_OVERRIDE=$m -c src/hw/hw_pwm.c -o "$OUT/ov$m.o" 2>"$OUT/ov$m.log"
    rc=$?
    w=$(grep -c -i "warning" "$OUT/ov$m.log")
    if [ $rc -eq 0 ] && [ $w -eq 0 ]; then
        echo "  [PASS] override=$m rc=0 warnings=0"; PASS=$((PASS+1))
    else
        echo "  [FAIL] override=$m rc=$rc warnings=$w"; sed 's/^/    /' "$OUT/ov$m.log" | head -10; FAIL=$((FAIL+1))
    fi
done

echo "================ 3. freestanding 双交叉编译 ================"
# ⚠️ 配方来自 tools/dmc_matrix.sh (已跑通的既有模块), 【不要加 -nostdinc】:
#    上一轮自行加的 -nostdinc 连 stdint.h 都找不到, 六模式全 rc=1 ——
#    那是命令行配方问题, 不是模块问题。既有模块能过, 说明配方本就如此。
R32="$HOME/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20230928/riscv32-esp-elf/bin"
XT="$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin"
for pair in "riscv32:$R32/riscv32-esp-elf-gcc" "xtensa:$XT/xtensa-esp-elf-gcc"; do
    name="${pair%%:*}"; gcc="${pair#*:}"
    if [ ! -x "$gcc" ]; then echo "  [SKIP] $name 交叉编译器缺失: $gcc"; continue; fi
    "$gcc" -std=c11 -Wall -Wextra -ffreestanding -Iinclude -Isrc -c src/hw/hw_pwm.c -o "$OUT/fs_$name.o" 2>"$OUT/fs_$name.log"
    rc=$?; w=$(grep -c -i "warning" "$OUT/fs_$name.log")
    if [ $rc -eq 0 ] && [ $w -eq 0 ]; then
        echo "  [PASS] freestanding $name rc=0 warnings=0"; PASS=$((PASS+1))
    else
        echo "  [FAIL] freestanding $name rc=$rc warnings=$w"; sed 's/^/    /' "$OUT/fs_$name.log" | head -15; FAIL=$((FAIL+1))
    fi
done

# ---------------------------------------------------------------------------
# ⭐ 4. 六模式逐一 C11 vs C++17 行为对拍
#
#    缺口性质: 第 1 项不带模式宏, 只覆盖 HOST 模式。而真机(ESP32-S3)跑的
#    是 override=3 那条分支, 它的代码路径从未在两形态下对拍过。
#    "编译通过"不等于"两形态行为相同" —— 这正是全跨式的核心命题。
#    判据: 同一模式内, C11 与 C++17 两套二进制跑同一组子命令, md5 必须逐位一致。
#    跨模式之间输出【允许】不同(模式名本就随模式变), 故只在模式内对拍。
# ---------------------------------------------------------------------------
echo "================ 4. 六模式逐一 C11 vs C++17 行为对拍 (补缺口) ================"
MODES=(HOST LINUX KELL ESP32 ESP8266 TEST)
for m in 0 1 2 3 4 5; do
    cc  -std=c11   -Wall -Wextra $INC -DHW_PWM_MODE_OVERRIDE=$m \
        -o "$OUT/m${m}_c11" $SRCS 2>"$OUT/m${m}_c11.log";   a=$?
    c++ -std=c++17 -Wall -Wextra $INC -x c++ -DHW_PWM_MODE_OVERRIDE=$m \
        -o "$OUT/m${m}_cpp" $SRCS 2>"$OUT/m${m}_cpp.log";   b=$?
    wa=$(grep -c -i "warning" "$OUT/m${m}_c11.log")
    wb=$(grep -c -i "warning" "$OUT/m${m}_cpp.log")
    if [ $a -ne 0 ] || [ $b -ne 0 ]; then
        echo "  [FAIL] mode=$m(${MODES[$m]}) 编译失败 c11=$a cpp=$b"
        sed 's/^/    /' "$OUT/m${m}_c11.log" "$OUT/m${m}_cpp.log" | head -10
        FAIL=$((FAIL+1)); continue
    fi
    run_cmds "$OUT/m${m}_c11" "$OUT/m${m}_c11.txt"
    run_cmds "$OUT/m${m}_cpp" "$OUT/m${m}_cpp.txt"
    ma=$(md5 -q "$OUT/m${m}_c11.txt"); mb=$(md5 -q "$OUT/m${m}_cpp.txt")
    MODE_MD5[$m]="$ma"
    if [ "$ma" = "$mb" ] && [ "$wa" -eq 0 ] && [ "$wb" -eq 0 ]; then
        echo "  [PASS] mode=$m(${MODES[$m]}) C11==C++17 md5=$ma 零警告"; PASS=$((PASS+1))
    else
        echo "  [FAIL] mode=$m(${MODES[$m]}) 形态不一致 或有警告  c11=$ma(w=$wa) cpp=$mb(w=$wb)"
        diff "$OUT/m${m}_c11.txt" "$OUT/m${m}_cpp.txt" | head -20
        FAIL=$((FAIL+1))
    fi
done

# ---------------------------------------------------------------------------
# 5. ⭐⭐ 黄金锁: 实测输出 vs 记录在案的 tools/pwm_golden.txt
#
#    为什么必须有: 第 1/4 项只验 "C11 == C++17", 是【自一致】, 不是"与记录一致"。
#    ⇒ 源码一改, 输出随之改变, 矩阵照样全 PASS, 而知识页记录的 md5 静默烂掉。
#    本项把"记录"钉进仓库, 使腐烂成为 FAIL 而非静默漂移。
#    处置: 有意改动 → bash tools/pwm_matrix.sh --update-golden 并在提交说明写清;
#          绝不手改 golden 消警告 (那等于把腐烂再钉一次)。
# ---------------------------------------------------------------------------
echo "================ 5. 黄金锁 (实测 vs 记录在案) ================"
if [ $UPDATE_GOLDEN -eq 1 ]; then
    { echo "# 自动生成于 $(date '+%Y-%m-%d %H:%M') —— bash tools/pwm_matrix.sh --update-golden"
      echo "# 语义: 见 tools/pwm_matrix.sh 第 5 项说明"
      echo "host  $H_MD5"
      for m in 0 1 2 3 4 5; do echo "mode$m ${MODE_MD5[$m]}"; done
    } > "$GOLDEN"
    echo "  [PASS] 黄金档已更新 -> $GOLDEN"
    PASS=$((PASS+1))
elif [ ! -f "$GOLDEN" ]; then
    echo "  [FAIL] 缺黄金档 $GOLDEN (先跑: bash tools/pwm_matrix.sh --update-golden)"; FAIL=$((FAIL+1))
else
    stale=0
    if ! grep -qx "host  $H_MD5" "$GOLDEN"; then
        echo "  [FAIL] host 漂移: 记录 $(grep '^host ' "$GOLDEN" | awk '{print $2}') / 实测 $H_MD5"; stale=1
    fi
    for m in 0 1 2 3 4 5; do
        cur="${MODE_MD5[$m]}"
        [ -z "$cur" ] && continue   # 编译已失败, 由第 4 项负责报
        rec=$(grep "^mode$m " "$GOLDEN" | awk '{print $2}')
        if [ "$rec" != "$cur" ]; then
            echo "  [FAIL] mode$m(${MODES[$m]}) 漂移: 记录 ${rec:-<无>} / 实测 $cur"; stale=1
        fi
    done
    if [ $stale -eq 0 ]; then
        echo "  [PASS] 7 项指纹与记录逐位一致 (host + 六模式)"; PASS=$((PASS+1))
    else
        echo "  ---- 处置: 若这是有意改动 -> bash tools/pwm_matrix.sh --update-golden"
        echo "        若非有意改动     -> 源码被动了而记录没跟上, 查 git diff"; FAIL=$((FAIL+1))
    fi
fi

# ---------------------------------------------------------------------------
# 6. ⭐⭐⭐ 独立 Python 黄金对拍
#
#    缺口性质: C 侧 selftest 断言的是 pwm_fnv32_reg_raw() == PWM_GOLDEN_REG,
#    期望与实现【同源】。若两者同时被改错, 断言恒成立 —— 死断言。
#    本项从 FNV-1a-32 规范独立重算, 且只从真头抽常量, 不复用 C 的任何代码。
#    它与第 5 项互补: 第 5 项防"记录腐烂", 本项防"实现与期望同源漂移"。
# ---------------------------------------------------------------------------
echo "================ 6. 独立 Python 黄金对拍 ================"
if command -v python3 >/dev/null 2>&1; then
    if python3 "$HERE/pwm_golden.py" --verbose; then
        PASS=$((PASS+1))
    else
        echo "  [FAIL] 独立口径算出与记录不一致 (rc=$?)"; FAIL=$((FAIL+1))
    fi
else
    echo "  [SKIP] 无 python3, 独立对拍未执行"; FAIL=$((FAIL+1))
fi

echo "================================================================"
echo "  PWM 全跨矩阵: PASS=$PASS FAIL=$FAIL"
exit $FAIL
