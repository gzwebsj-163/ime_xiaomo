#!/usr/bin/env bash
# ============================================================================
# hw_wdbg 全跨式验证矩阵 (2026-10-02 补)
#
#   动机: dmc 有专属矩阵, wdbg 一直只靠 `make test` 里的宿主 C++17 一条腿。
#         本轮把 g_bsp 从「指针」改成「结构体 + g_bsp_set」= 数据结构改动,
#         而结构体改动正是 C 形态/交叉编译的风险区 —— 却从未在 C11 形态下
#         编译过 hw_wdbg.c。锚点: 自证工具与被测物共享盲点。
#
#   1. C11 vs C++17 输出 md5 逐位对拍 (HOST 模式)
#   2. 六模式 override 编译零警告
#   3. freestanding 双交叉 (riscv32 / xtensa) 零警告
#   4. ⭐ 六模式逐一 C11 vs C++17 行为对拍
#      —— 2/3 只验"能编译", 不验"两形态行为相同"。真机跑的是 ESP32 分支,
#         每个模式实际编成两套二进制对拍输出, #else 分支有形态差异即被抓出。
#
#   ⚠️ 唯一「允许」的警告: wd_install_stack_bsp() 返回栈上局部地址
#      (selftest [18] 判别式**故意**制造该形状, 它是被测的缺陷形状本身,
#      不是 bug)。本脚本对它做显式豁免, 出现别的警告即 FAIL。
# ============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1

OUT=tmp/wdbg_matrix
mkdir -p "$OUT"
INC="-Iinclude -Isrc"
# 纯转发驱动 + 模块本体 (不含 src/main.c, 免得把整个 VM 掺进双形态对拍)
SRCS="src/hw/hw_wdbg.c tests/wdbg_host_main.c"
PASS=0; FAIL=0
# 判定: 剥掉允许的警告行后, 还剩几条 warning
# ⚠️ 两种编译器的措辞不同, 都要豁免:
#    clang: "address of stack memory associated with local variable 'local' returned"
#    gcc  : "function returns address of local variable"
#    (只写一种 → 另一种编译器的警告被当成真警告 → 13 个假 FAIL 的来源之一)
ALLOW_RE='wd_install_stack_bsp|return-stack-address|returns address of local variable|address of stack memory|warning generated\.'
# 上面最后一段 "warning generated\." 是 clang 的**汇总行** (如 "1 warning generated.") ——
# 它不含警告正文, 若不豁免会把上面已豁免的同一条警告**重复计数**成假 FAIL。
# 同理豁免 -Wdeprecated: 那是因为 .c 文件被直接丢给 c++ 驱动的脚手架噪音, 与被测物无关。

# 只把 .c 重命名成 .cpp 再喂 c++ 驱动, 避免 "treating 'c' input as 'c++'" 噪音。
# 纪律: 双形态对拍要比的是**被测代码**, 不是驱动对文件后缀的抱怨。
cpp_srcs() {
  local d="$OUT/cpp_src"; rm -rf "$d"; mkdir -p "$d"
  local out=""
  for f in $SRCS; do
    cp "$f" "$d/$(basename "${f%.c}").cpp"
    out="$out $d/$(basename "${f%.c}").cpp"
  done
  echo "$out"
}

# 判定: 剥掉允许的警告行后, 还剩几条 warning
warn_count() { grep -i "warning" "$1" | grep -vE "$ALLOW_RE" | wc -l | tr -d ' '; }

echo "================ 1. C11 vs C++17 输出 md5 逐位对拍 (HOST 模式) ================"
cc    -std=c11   -Wall -Wextra $INC -o "$OUT/wdbg_c11"   $SRCS 2>"$OUT/c11.log"
c++   -std=c++17 -Wall -Wextra $INC -o "$OUT/wdbg_cpp17" $(cpp_srcs) 2>"$OUT/cpp17.log"
w1=$(warn_count "$OUT/c11.log"); w2=$(warn_count "$OUT/cpp17.log")
echo "  C11    build 允许外警告=$w1"
echo "  C++17  build 允许外警告=$w2"
if [ ! -x "$OUT/wdbg_c11" ] || [ ! -x "$OUT/wdbg_cpp17" ]; then
  echo "  [FAIL] 编译失败"; sed 's/^/    /' "$OUT/c11.log" "$OUT/cpp17.log"; FAIL=$((FAIL+1))
elif [ "$w1" != "0" ] || [ "$w2" != "0" ]; then
  echo "  [FAIL] 存在允许外的警告"; FAIL=$((FAIL+1))
else
  # 确定性子命令 (纯读路径/卡/校验和, 无时间/随机量)
  : > "$OUT/c11.txt"; : > "$OUT/cpp17.txt"
  for c in selftest card; do
    "$OUT/wdbg_c11"   wdbg "$c" >>"$OUT/c11.txt"   2>&1
    "$OUT/wdbg_cpp17" wdbg "$c" >>"$OUT/cpp17.txt" 2>&1
  done
  m1=$(md5 -q "$OUT/c11.txt"); m2=$(md5 -q "$OUT/cpp17.txt")
  echo "  C11    out md5=$m1"
  echo "  C++17  out md5=$m2"
  if [ "$m1" = "$m2" ]; then
    echo "  [PASS] C11 == C++17 逐位一致, 两端零警告"; PASS=$((PASS+1))
  else
    echo "  [FAIL] 输出不一致"; diff "$OUT/c11.txt" "$OUT/cpp17.txt" | head -20; FAIL=$((FAIL+1))
  fi
fi

echo
echo "================ 2. 六模式 override 编译零警告 ================"
for m in 0 1 2 3 4 5; do
  cc -std=c11 -Wall -Wextra $INC -DHW_WDBG_MODE_OVERRIDE=$m -c src/hw/hw_wdbg.c -o "$OUT/ov$m.o" 2>"$OUT/ov$m.log"
  w=$(warn_count "$OUT/ov$m.log")
  if [ "$w" = "0" ]; then echo "  [PASS] mode=$m 零警告"; PASS=$((PASS+1))
  else echo "  [FAIL] mode=$m 警告 $w 条"; grep -i warning "$OUT/ov$m.log" | grep -vE "$ALLOW_RE" | head; FAIL=$((FAIL+1)); fi
done

echo
echo "================ 3. freestanding 双交叉 ================"
R32="$HOME/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20230928/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"
XT="$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin/xtensa-esp-elf-gcc"
for pair in "riscv32:$R32" "xtensa:$XT"; do
  name="${pair%%:*}"; gcc="${pair#*:}"
  if [ ! -x "$gcc" ]; then echo "  [SKIP] $name 交叉编译器缺失: $gcc"; continue; fi
  "$gcc" -std=c11 -Wall -Wextra -ffreestanding $INC -c src/hw/hw_wdbg.c -o "$OUT/fs_$name.o" 2>"$OUT/fs_$name.log"
  w=$(warn_count "$OUT/fs_$name.log")
  if [ "$w" = "0" ]; then echo "  [PASS] $name freestanding 零警告 (仅保留判别式刻意警告)"; PASS=$((PASS+1))
  else echo "  [FAIL] $name 警告 $w 条"; grep -i warning "$OUT/fs_$name.log" | grep -vE "$ALLOW_RE" | head; FAIL=$((FAIL+1)); fi
done

echo
echo "================ 4. ⭐ 六模式逐一 C11 vs C++17 行为对拍 ================"
for m in 0 1 2 3 4 5; do
  cc  -std=c11   -Wall -Wextra $INC -DHW_WDBG_MODE_OVERRIDE=$m -o "$OUT/m${m}_c11"   $SRCS 2>/dev/null
  c++ -std=c++17 -Wall -Wextra $INC -DHW_WDBG_MODE_OVERRIDE=$m -o "$OUT/m${m}_cpp17" $(cpp_srcs) 2>/dev/null
  if [ ! -x "$OUT/m${m}_c11" ] || [ ! -x "$OUT/m${m}_cpp17" ]; then
    echo "  [FAIL] mode=$m 编译失败"; FAIL=$((FAIL+1)); continue
  fi
  : > "$OUT/m${m}_c11.txt"; : > "$OUT/m${m}_cpp17.txt"
  for c in selftest card; do
    "$OUT/m${m}_c11"   wdbg "$c" >>"$OUT/m${m}_c11.txt"   2>&1
    "$OUT/m${m}_cpp17" wdbg "$c" >>"$OUT/m${m}_cpp17.txt" 2>&1
  done
  a=$(md5 -q "$OUT/m${m}_c11.txt"); b=$(md5 -q "$OUT/m${m}_cpp17.txt")
  if [ "$a" = "$b" ]; then echo "  [PASS] mode=$m C11 == C++17  md5=$a"; PASS=$((PASS+1))
  else echo "  [FAIL] mode=$m 不一致 $a vs $b"; diff "$OUT/m${m}_c11.txt" "$OUT/m${m}_cpp17.txt" | head -10; FAIL=$((FAIL+1)); fi
done

echo
echo "================  汇总  ================"
echo "  通过: $PASS   失败: $FAIL"
[ "$FAIL" = "0" ] && echo "  全部通过 ✔" || echo "  有失败 ✘"
exit "$FAIL"
