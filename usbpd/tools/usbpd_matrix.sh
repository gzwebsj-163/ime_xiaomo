#!/usr/bin/env bash
# ============================================================================
# hw_usbpd 全跨式验证矩阵 (不采信自己, 每项独立口径)
#
#   1. C11 vs C++17 输出 md5 逐位对拍 (宿主导动, 走真 CLI 入口)
#   2. freestanding 双交叉编译 (riscv32-esp-elf / xtensa-esp-elf)
#   3. BSP 边界探针 SIM/REAL 分水岭 + 分压还原往返 (C11 / C++17 各跑一次)
#   4. 黄金值独立口径对拍 (python3 与 C 各自算出同一个数)
#   5. ⭐ 变异对照 (每条新守卫都有变异打它)
#
# ⚠️ 为什么【没有】"六模式 override" 这一项 —— 别照抄家族规约
#   兄弟模块 (dmc/dc/wdbg/...) 都有 -DHW_XXX_MODE_OVERRIDE=0..5, 矩阵里
#   有一项"六模式编译零警告 + 六模式行为对拍"。本模块【没有】这个宏:
#     · usbpd 源码里 #if/#ifdef/#else 形态守卫计数 = 0
#     · MODE_OVERRIDE 计数 = 0
#   没有可 override 的东西时, 硬造一个"六模式"测试只会得到一个
#   【恒真的空测试】= 绿灯, 但它对"真机上跑的分支是否两形态一致"
#   一点信息量都没有 (锚点 H: 恒真信号比假信号更险, 因为它让你"有依据地"放心)。
#   本矩阵用【BSP 注入前后的行为对拍】替代: 它验证的是本模块真正的
#   承重结构 (SIM/REAL 分水岭), 而不是编造出来的模式编号。
#   若将来 usbpd 真的长出形态守卫, 必须同时把这一项加回来。
#
# 位置: 本脚本放在 usbpd/tools/ (规范位置), 不寄居 tmp/ —— tmp 是易清理区,
#   验证工具寄居临时目录 = 下次复现时它可能已经不在了 (dmc 踩过这个坑)。
# ============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/tmp/usbpd_matrix"
INC="-I$ROOT/include -I$ROOT/usbpd/include"
SRC="$ROOT/usbpd/src/hw_usbpd.c"
ST="$ROOT/usbpd/src/hw_usbpd_selftest.c"
HOST="$ROOT/usbpd/tools/usbpd_host_main.c"
PROBE="$ROOT/usbpd/tools/usbpd_bsp_probe.c"
mkdir -p "$OUT"
cd "$ROOT"

PASS=0; FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

# 固定命令序列: 全部读路径, 无时间/随机量, 两形态输出必须逐位一致
CMDS=(selftest golden rows tol at_2700 at_1200 at_0 at_2000 zzz)

# ⚠️ 【探针自带口径, 不复用产物的输出】cmds 里不直接放 "at 2700",
#   因为 cmd 里带空格 —— 由 run_cmds() 用数组正确传参。
run_cmds() {
  local bin="$1"
  for c in "${CMDS[@]}"; do
    case "$c" in
      at_*) echo "--- ${c#at_} ---"; "$bin" usbpd "at" "${c#at_}" 2>&1; echo "rc=$?" ;;
      *)    echo "--- $c ---"; "$bin" usbpd "$c" 2>&1; echo "rc=$?" ;;
    esac
  done
}

echo "================ 1. C11 vs C++17 输出 md5 逐位对拍 ================"
cc  -std=c11   -Wall -Wextra $INC -x c   -o "$OUT/host_c11"  $SRC $ST $HOST 2>"$OUT/c11.log";   r1=$?
c++ -std=c++17 -Wall -Wextra $INC -x c++ -o "$OUT/host_cpp17" $SRC $ST $HOST 2>"$OUT/cpp17.log"; r2=$?
# ⚠️ 判据自身曾经是坏的(锚点 J 家族): `grep -c` 无匹配时【已经输出 0】但退出码=1,
#   所以 `|| echo 0` 会再追加一个 0 → 变量变成 "0\n0" → `[ "$w" -eq 0 ]` 报
#   "integer expression expected" → 真 PASS 被判成 FAIL。修法: 不在已有输出的地方再 echo,
#   用 ${w:-0} 兜住"文件不存在→grep 无输出"的情形。
w1=$(grep -c "warning:" "$OUT/c11.log"   2>/dev/null); w1=${w1:-0}
w2=$(grep -c "warning:" "$OUT/cpp17.log" 2>/dev/null); w2=${w2:-0}
echo "  C11   build rc=$r1 warnings=$w1"
echo "  C++17 build rc=$r2 warnings=$w2"
if [ $r1 -eq 0 ] && [ $r2 -eq 0 ] && [ "$w1" -eq 0 ] && [ "$w2" -eq 0 ]; then
  run_cmds "$OUT/host_c11"   > "$OUT/out_c11.txt" 2>&1
  run_cmds "$OUT/host_cpp17" > "$OUT/out_cpp17.txt" 2>&1
  m1=$(md5 -q "$OUT/out_c11.txt"); m2=$(md5 -q "$OUT/out_cpp17.txt")
  echo "  C11   out md5=$m1"
  echo "  C++17 out md5=$m2"
  if [ "$m1" = "$m2" ]; then ok "C11 == C++17 逐位一致, 两端零警告"
  else bad "两形态输出不一致 —— 逐位 diff:"; diff "$OUT/out_c11.txt" "$OUT/out_cpp17.txt" | head -20 | sed 's/^/    /'; fi
else
  bad "构建失败或出警告 (rc=$r1/$r2 warn=$w1/$w2)"; head -15 "$OUT/c11.log" "$OUT/cpp17.log" | sed 's/^/    /'
fi

echo "================ 2. freestanding 双交叉编译 ================"
RV=$(ls "$HOME"/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc 2>/dev/null | head -1)
TX=$(ls "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | head -1)
for pair in "riscv32:$RV" "xtensa-s3:$TX"; do
  name="${pair%%:*}"; gcc="${pair#*:}"
  if [ -z "$gcc" ] || [ ! -x "$gcc" ]; then bad "freestanding $name: 交叉编译器不可用 (跳过)"; continue; fi
  "$gcc" -std=c11 -Wall -Wextra -ffreestanding $INC -c "$SRC" -o "$OUT/fs_$name.o" 2>"$OUT/fs_$name.log"; rc=$?
  w=$(grep -c "warning:" "$OUT/fs_$name.log" 2>/dev/null); w=${w:-0}
  if [ $rc -eq 0 ] && [ "$w" -eq 0 ]; then ok "freestanding $name rc=0 warnings=0"
  else bad "freestanding $name rc=$rc warnings=$w"; head -15 "$OUT/fs_$name.log" | sed 's/^/    /'; fi
done

echo "================ 3. BSP 边界探针 (SIM/REAL 分水岭 + 还原往返) ================"
for form in c11 cpp17; do
  if [ "$form" = c11 ]; then CC="cc -std=c11 -x c"; B="$OUT/probe_c11"
  else CC="c++ -std=c++17 -x c++"; B="$OUT/probe_cpp17"; fi
  # ⚠️ 必须带 $ST: hw_usbpd.c 的 hw_usbpd_cmd() 引用 hw_usbpd_selftest,
  #   C++17 下 extern "C" 修饰后 mangle 成 _hw_usbpd_selftest,
  #   漏掉 selftest 翻译单元 = 链接期 Undefined symbols, 探针根本跑不起来。
  $CC -Wall -Wextra $INC -o "$B" $SRC $ST $PROBE 2>"$OUT/probe_$form.log"; rc=$?
  if [ $rc -ne 0 ]; then bad "探针($form) 构建失败"; head -15 "$OUT/probe_$form.log" | sed 's/^/    /'; continue; fi
  out=$("$B" 2>&1); prc=$?
  # ⚠️ 判据口径必须与探针【实际打印格式】逐字对齐: 探针 CHECK() 打印的是
  #   "  ok   " / "  FAIL " (无方括号)。曾经 grep '\[OK\]' / '\[FAIL\]' →
  #   恒数到 0 = 两个恒真判据, 报告显示"断言数=0 失败=0"却照样 PASS。
  #   失败行以两空格+F 开头; "RESULT: FAILED" 以 R 开头, 故 '^  FAIL' 不会误计它。
  #   真正的兜底承重判据是 prc(=探针 return g_fail), 这一路与 grep 独立。
  nf=$(echo "$out" | grep -c "^  FAIL"); nf=${nf:-0}
  na=$(echo "$out" | grep -c "^  ok");   na=${na:-0}
  echo "  $form 探针: 断言数=$na 失败=$nf 退出码=$prc"
  if [ "$nf" -eq 0 ] && [ "$prc" -eq 0 ]; then ok "BSP 边界探针 ($form) 全绿 ($na 断言)"
  else bad "BSP 边界探针 ($form) 有 $nf 项失败 (退出码=$prc)"; echo "$out" | grep "^  FAIL" | head -8 | sed 's/^/    /'; fi
done
# 两形态探针输出也必须逐位一致
if [ -x "$OUT/probe_c11" ] && [ -x "$OUT/probe_cpp17" ]; then
  p1=$("$OUT/probe_c11" 2>&1 | grep -v "^$" | md5 -q)
  p2=$("$OUT/probe_cpp17" 2>&1 | grep -v "^$" | md5 -q)
  [ "$p1" = "$p2" ] && ok "探针 C11 == C++17 输出一致" || bad "探针两形态输出不一致"
fi

echo "================ 4. 黄金值独立口径对拍 (python3 vs C) ================"
# 🕳️【格式对齐, 别拿不同表示法硬比】这一项我第一次写成
#   `python3 golden_calc.py | tr -d ' \n'` 直接和 C 的输出比字符串 ——
#   两个来源的【表示法根本不同】, 那样比是垃圾对比:
#     · python 侧 golden_calc.py 打印的是【多行报告】, 里面有
#       "FNV-1a-32 over <Iii> = 0xC481F6E5" 这样一行, 不是裸 hex;
#     · C 侧 cmd("golden") 是 `return (int)HW_USBPD_GOLDEN`, 而
#       HW_USBPD_GOLDEN 定义为 0xC481F6E5u (最高位=1) ⇒ (int) 转换后
#       变成【负数】-998115611 打印出来。
#   同一个值, 两种写法 ⇒ 字符串必然不等 ⇒ 绿灯变红灯, 而红灯的原因
#   不在产品, 在我写的比法 (锚点 F: 我没在看 ≠ 口径不同)。
#   正解: 各自抽出数值再比 —— python 抽 hex, C 抽十进制, 统一到有符号
#   int32 口径。
if command -v python3 >/dev/null 2>&1 && [ -f "$ROOT/usbpd/tools/golden_calc.py" ]; then
  pyg=$(python3 "$ROOT/usbpd/tools/golden_calc.py" 2>/dev/null \
        | grep -oE 'FNV-1a-32 over <Iii> = 0x[0-9A-Fa-f]+' | grep -oE '0x[0-9A-Fa-f]+')
  cgold=$("$OUT/host_c11" usbpd golden 2>/dev/null | head -1 | tr -d ' \r')
  echo "  python3 独立算出 : ${pyg:-<没抓到 hex 行>}"
  echo "  C 运行时返回     : ${cgold:-<空>}"
  if [ -z "$pyg" ] || [ -z "$cgold" ]; then
    bad "黄金值对拍取不到数 (py='${pyg}' c='${cgold}') —— 空值按【失配】处理, 不当通过"
  else
    py_signed=$(python3 -c "v=int('${pyg}',16); print(v-(1<<32) if v>=(1<<31) else v)")
    echo "  统一到 int32 口径: py=$py_signed  c=$cgold"
    if [ "$py_signed" = "$cgold" ]; then ok "黄金值两口径一致 (0x${pyg#0x} == $cgold)"
    else bad "黄金值不一致 (py=$py_signed c=$cgold)"; fi
  fi
else
  bad "golden_calc.py 缺失或 python3 不可用"
fi

echo "================ 5. 变异对照 (每条守卫都要有变异打它) ================"
if python3 "$ROOT/usbpd/tools/mutate_test.py" > "$OUT/mut.log" 2>&1; then
  grep -E "变异 15 个|崩溃杀" "$OUT/mut.log" | sed 's/^/  /'
  ok "变异对照全杀 (0 存活)"
else
  bad "变异对照有存活/跳过:"; grep -E "SURVIVED|SKIP|变异 15 个" "$OUT/mut.log" | sed 's/^/    /'
fi

echo ""
echo "================ 汇总: PASS=$PASS FAIL=$FAIL ================"
[ $FAIL -eq 0 ] || exit 1
exit 0
