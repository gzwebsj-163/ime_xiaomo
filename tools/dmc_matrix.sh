#!/usr/bin/env bash
# ============================================================================
# hw_dmc 全跨式验证矩阵 (不采信自己, 每项独立口径)
#
#   1. C11 vs C++17 (默认/HOST 模式) : 同源编译 → 跑同一组确定性子命令 → 输出 md5 逐位一致
#   2. 六模式 override 编译零警告     : -DHW_DMC_MODE_OVERRIDE=0..5 各自独立编译
#   3. freestanding 双交叉            : riscv32-esp-elf / xtensa-esp-elf 加 -ffreestanding
#   4. ⭐ 六模式逐一 C11 vs C++17 行为对拍 (2026-10-01 补)
#      —— 1 只证明了 HOST 模式的双形态一致, 而真机跑的是 ESP32 模式分支;
#         2/3 只验"能编译", 不验"两形态行为相同"。本项把每个模式的 .c 实际编译成
#         两套二进制并对拍输出, 任何一个模式的 #else 分支有形态差异都会被抓出来。
#
# 位置说明: 本脚本此前寄居 tmp/dmc_matrix/matrix.sh —— tmp 是易清理区, 验证工具
# 寄居临时目录 = 下次复现时它可能已经不在了。已迁到 tools/ 作为规范位置。
# ============================================================================
set -u
# 路径按脚本自身位置推导 —— 原先写死 $HOME/..., 在别的机器/别的用户下直接失效。
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1
OUT=tmp/dmc_matrix
GOLDEN="$HERE/dmc_golden.txt"
UPDATE_GOLDEN=0
[ "${1:-}" = "--update-golden" ] && UPDATE_GOLDEN=1
mkdir -p "$OUT"
SRCS="src/hw/hw_dmc.c src/hw/hw_dmc_base.c tests/dmc_host_main.c"
INC="-Iinclude -Isrc"
PASS=0; FAIL=0
H_MD5=""; MODE_MD5=()

# 固定子命令序列 (全部读路径, 无时间/随机量)
CMDS=(selftest cmds states golden frame crcvec hello)

echo "================ 1. C11 vs C++17 输出 md5 逐位对拍 (默认/HOST 模式) ================"
cc  -std=c11   -Wall -Wextra $INC -o "$OUT/dmc_c11"   $SRCS 2>"$OUT/c11.log"
r1=$?
c++ -std=c++17 -Wall -Wextra $INC -x c++ -o "$OUT/dmc_cpp17" $SRCS 2>"$OUT/cpp17.log"
r2=$?
warn1=$(grep -c -i "warning" "$OUT/c11.log")
warn2=$(grep -c -i "warning" "$OUT/cpp17.log")
echo "  C11  build rc=$r1 warnings=$warn1"
echo "  C++17 build rc=$r2 warnings=$warn2"
if [ $r1 -ne 0 ] || [ $r2 -ne 0 ]; then
    echo "  [FAIL] 编译失败"; sed 's/^/    /' "$OUT/c11.log" "$OUT/cpp17.log"; FAIL=$((FAIL+1))
else
    : > "$OUT/c11.txt"; : > "$OUT/cpp17.txt"
    for c in "${CMDS[@]}"; do
        "$OUT/dmc_c11"   dmc "$c" >>"$OUT/c11.txt"   2>&1
        "$OUT/dmc_cpp17" dmc "$c" >>"$OUT/cpp17.txt" 2>&1
    done
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
    cc -std=c11 -Wall -Wextra $INC -DHW_DMC_MODE_OVERRIDE=$m -c src/hw/hw_dmc.c -o "$OUT/ov$m.o" 2>"$OUT/ov$m.log"
    rc=$?
    w=$(grep -c -i "warning" "$OUT/ov$m.log")
    if [ $rc -eq 0 ] && [ $w -eq 0 ]; then
        echo "  [PASS] override=$m rc=0 warnings=0"; PASS=$((PASS+1))
    else
        echo "  [FAIL] override=$m rc=$rc warnings=$w"; sed 's/^/    /' "$OUT/ov$m.log" | head -10; FAIL=$((FAIL+1))
    fi
done

echo "================ 3. freestanding 双交叉编译 ================"
R32="$HOME/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20230928/riscv32-esp-elf/bin"
XT="$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin"
for pair in "riscv32:$R32/riscv32-esp-elf-gcc" "xtensa:$XT/xtensa-esp-elf-gcc"; do
    name="${pair%%:*}"; gcc="${pair#*:}"
    if [ ! -x "$gcc" ]; then echo "  [SKIP] $name 交叉编译器缺失: $gcc"; continue; fi
    "$gcc" -std=c11 -Wall -Wextra -ffreestanding -Iinclude -Isrc -c src/hw/hw_dmc.c -o "$OUT/fs_$name.o" 2>"$OUT/fs_$name.log"
    rc=$?; w=$(grep -c -i "warning" "$OUT/fs_$name.log")
    if [ $rc -eq 0 ] && [ $w -eq 0 ]; then
        echo "  [PASS] freestanding $name rc=0 warnings=0"; PASS=$((PASS+1))
    else
        echo "  [FAIL] freestanding $name rc=$rc warnings=$w"; sed 's/^/    /' "$OUT/fs_$name.log" | head -15; FAIL=$((FAIL+1))
    fi
done

# ---------------------------------------------------------------------------
# ⭐ 4. 六模式逐一 C11 vs C++17 行为对拍 —— 补 2026-10-01 发现的验证缺口
#
#    缺口性质: 第 1 项不带任何模式宏, 只覆盖 HOST 模式。而真机(ESP32-S3)跑的
#    是 override=3 那条分支, 它的 #else 代码路径从未在两形态下对拍过。
#    "编译通过"不等于"两形态行为相同" —— 这正是全跨式的核心命题。
#
#    判据: 同一模式内, C11 与 C++17 两套二进制跑同一组子命令, 输出 md5 必须逐位一致。
#    跨模式之间输出**允许**不同(模式名等本就随模式变), 故只在模式内对拍。
# ---------------------------------------------------------------------------
echo "================ 4. 六模式逐一 C11 vs C++17 行为对拍 (补缺口) ================"
MODES=(HOST LINUX KELL ESP32 ESP8266 TEST)
for m in 0 1 2 3 4 5; do
    cc  -std=c11   -Wall -Wextra $INC -DHW_DMC_MODE_OVERRIDE=$m \
        -o "$OUT/m${m}_c11" $SRCS 2>"$OUT/m${m}_c11.log";   a=$?
    c++ -std=c++17 -Wall -Wextra $INC -x c++ -DHW_DMC_MODE_OVERRIDE=$m \
        -o "$OUT/m${m}_cpp" $SRCS 2>"$OUT/m${m}_cpp.log";   b=$?
    wa=$(grep -c -i "warning" "$OUT/m${m}_c11.log")
    wb=$(grep -c -i "warning" "$OUT/m${m}_cpp.log")
    if [ $a -ne 0 ] || [ $b -ne 0 ]; then
        echo "  [FAIL] mode=$m(${MODES[$m]}) 编译失败 c11=$a cpp=$b"
        sed 's/^/    /' "$OUT/m${m}_c11.log" "$OUT/m${m}_cpp.log" | head -10
        FAIL=$((FAIL+1)); continue
    fi
    : > "$OUT/m${m}_c11.txt"; : > "$OUT/m${m}_cpp.txt"
    for c in "${CMDS[@]}"; do
        "$OUT/m${m}_c11" dmc "$c" >>"$OUT/m${m}_c11.txt" 2>&1
        "$OUT/m${m}_cpp" dmc "$c" >>"$OUT/m${m}_cpp.txt" 2>&1
    done
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
# 5. ⭐⭐ 黄金锁: 实测输出 vs 记录在案的 tools/dmc_golden.txt
#
#    为什么必须有这一项 (本项目已实锤的血泪教训):
#      第 1/4 项只验 "C11 == C++17", 是**自一致**, 不是"与记录一致"。
#      ⇒ 源码一改 (例: 补 [20b]/[26] 两条 selftest 用例), 输出随之改变,
#        矩阵照样 PASS=15 FAIL=0, 而知识页记录的 md5 静默烂掉 ——
#        本页指纹已这样作废三次 (1d7295aa → 8398df8e → ef54f2a8)。
#      根因不是"没人跑矩阵", 而是**矩阵在设计上就无法发现记录腐烂**。
#      本项把"记录"钉进仓库, 使腐烂成为 FAIL 而非静默漂移。
#
#    处置: 源码有意改动 → bash tools/dmc_matrix.sh --update-golden 并在提交说明
#          里写清改动; 绝不手改 golden 消警告 (那等于把腐烂再钉一次)。
# ---------------------------------------------------------------------------
echo "================ 5. 黄金锁 (实测 vs 记录在案) ================"
if [ $UPDATE_GOLDEN -eq 1 ]; then
    { echo "# 自动生成于 $(date '+%Y-%m-%d %H:%M') —— bash tools/dmc_matrix.sh --update-golden"
      echo "# 语义: 见 tools/dmc_matrix.sh 第 5 项说明"
      echo "host  $H_MD5"
      for m in 0 1 2 3 4 5; do echo "mode$m ${MODE_MD5[$m]}"; done
    } > "$GOLDEN"
    echo "  [PASS] 黄金档已更新 -> $GOLDEN"
    PASS=$((PASS+1))
elif [ ! -f "$GOLDEN" ]; then
    echo "  [FAIL] 缺黄金档 $GOLDEN (先跑: bash tools/dmc_matrix.sh --update-golden)"; FAIL=$((FAIL+1))
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
        echo "  ---- 处置: 若这是有意改动 -> bash tools/dmc_matrix.sh --update-golden"
        echo "        若非有意改动     -> 源码被动了而记录没跟上, 查 git diff"; FAIL=$((FAIL+1))
    fi
fi

echo "================================================================"
echo "  DMC 全跨矩阵: PASS=$PASS FAIL=$FAIL"
exit $FAIL
