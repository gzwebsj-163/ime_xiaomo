#!/usr/bin/env bash
# ============================================================================
# hw_main 全跨式验证矩阵 (2026-10-03 补)
#
#   动机: hw_main 一直是 8/9 类的"总调度", 本轮登记 USBPD 后黄金值跨过
#         2^31 (0x5FAD755A -> 0xA57E74DF), 掩码类 bug 当场暴露并已修,
#         但 hw_main.c 从未在 C11 形态下编译过 —— 家族铁律"C11==C++17
#         逐位一致"在**总调度**这一层是空白。锚点: 自证工具与被测物共享盲点。
#
#   0.  桩的阳性对照 (9 探针全 OK)
#   0b. 桩的阴性对照 (8 个可判别类全 BAD)  <- 证明探针会红
#   0c. 逐探针独立对照 (一次只翻一个)      <- 证明各探针接对符号
#   1. C11 vs C++17 输出 md5 逐位对拍 (HOST 模式)
#   2. 六模式 override 编译零警告
#   3. freestanding 双交叉 (riscv32 / xtensa) 零警告
#   4. 六模式逐一 C11 vs C++17 行为对拍
#
#   ⚠️ 本矩阵【不链接真实家族模块】, 而用 tests/hwmain_stub_probe.c 供桩:
#      macOS ld64 不把 weak undefined 解析为 0 (clang+gcc 双验, 3 行最小复现),
#      hw_main.c 的 9 个弱符号必须有人提供定义, 否则链接失败;
#      而拉真模块会拖进整个 VM (hw_direct -> kvm_* / hw_oem -> kprog_*),
#      把与 hw_main 无关的变量掺进双形态对拍。
#      桩的【阳性对照】要求: 9 个探针必须全 OK (见下方 probeall 预检)。
#
#   ⚠️ 桩带来的已知盲区 (如实记, 不假装覆盖):
#      (a) NOLINK 分支在 macOS 上【结构性不可测】。实测 4 种链接旗标
#          (-U / dynamic_lookup / flat_namespace / undefined,suppress) 全部
#          仍报 undefined; 借 dylib 绕过去看似拿到 0, 但加 -rpath 或
#          DYLD_LIBRARY_PATH 后立刻变成非 0 —— 那 rc=7 是"dylib 没被加载"
#          的假象, 不是 NOLINK 语义。=> 结论: 真机/ELF 上才谈得上验 NOLINK。
#      (b) 真模块下的探针分支未经此矩阵验证 (同 (a) 的理由 + 拖 VM)。
#      段 0b/0c 补的是【探针逻辑本身】的区分力, 不能外推到链接期行为。
# ============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1

OUT=tmp/hwmain_matrix
mkdir -p "$OUT"
INC="-Iinclude -Isrc -Iusbpd/include"
# 🕳️ 汇总自校验需要"屏幕实际输出了几条 [PASS]"这个独立口径,
#    所以本脚本把自身输出同时落到 run.log, 再由汇总段数回去对撞。
#    纪律: 判据不能与被测物同源同一条路径, 否则两边一起错, 永远自洽。
exec > >(tee "$OUT/run.log") 2>&1

# 驱动 + 被测物 + 探针桩 (不含 src/main.c, 免得把整个 VM 掺进对拍)
SRCS="tests/hwmain_host_main.c src/hw/hw_main.c tests/hwmain_stub_probe.c"
PASS=0; FAIL=0

# 只把 .c 重命名成 .cpp 再喂 c++ 驱动, 避免 "treating 'c' input as 'c++'" 噪音。
# 纪律: 双形态对拍要比的是【被测代码】, 不是驱动对文件后缀的抱怨。
cpp_srcs() {
  local d="$OUT/cpp_src"; rm -rf "$d"; mkdir -p "$d"
  local out=""
  for f in $SRCS; do
    cp "$f" "$d/$(basename "${f%.c}").cpp"
    out="$out $d/$(basename "${f%.c}").cpp"
  done
  echo "$out"
}

warn_count() { grep -i "warning" "$1" | wc -l | tr -d ' '; }

# ⭐ 阳性对照: 桩若违约, 探针会报 BAD, 那测的就不是 hw_main 而是桩。
#    本函数在正式对拍前跑一次, 9 个探针非全 OK 直接 FAIL 全部。
probeall_ok() {
  "$1" main selftest 2>&1 | grep -c "probe=OK" | tr -d ' '
}

echo "================ 0. 桩的阳性对照 (9 探针须全 OK) ================"
cc -std=c11 $INC -o "$OUT/stubchk" $SRCS 2>"$OUT/stubchk.log"
if [ ! -x "$OUT/stubchk" ]; then
  echo "  [FAIL] 桩检查构建失败"; sed 's/^/    /' "$OUT/stubchk.log"; exit 1
fi
NP=$(probeall_ok "$OUT/stubchk")
echo "  probe=OK 计数 = $NP (应 = 9)"
if [ "$NP" = "9" ]; then
  echo "  [PASS] 桩使 9 个探针全 OK -> 阳性对照成立"; PASS=$((PASS+1))
else
  echo "  [FAIL] 桩违约, 探针非全 OK -> 对拍会测到桩而非被测物, 中止"
  "$OUT/stubchk" main selftest 2>&1 | sed 's/^/    /'
  exit 1
fi

echo
echo "================ 0b. 桩的阴性对照 (8 个可判别类须全 BAD) ================"
# 🕳️🕳️ 为什么段 0 的阳性对照【不够】(锚点 D / 锚点 H):
#   段 0 只跑了"桩让 9 个探针全绿"这一条路。于是哪怕 main_probe_dev 里那句
#   `return OK` 是恒真的、哪怕某个探针根本接错了符号, 矩阵照样全绿。
#   任何【只测过"好"】的探针都还没被验过 —— 本段就是那个"红"的证据。
#   预期 BAD=8 而不是 9: 2 号 DEV 的底层是 hw_dev_registered(), 一个
#   【无失败态的纯 getter】(hw_dev.c 直接 return dev_bin.count), 不存在
#   BAD 状态可言。这里不编造判据换假的区分力, 而是如实钉住"8 可判别 + 1 恒绿"。
cc -std=c11 -DSTUB_BAD_ALL=1 $INC -o "$OUT/stubbad" $SRCS 2>"$OUT/stubbad.log"
if [ ! -x "$OUT/stubbad" ]; then
  echo "  [FAIL] 阴性对照桩构建失败"; sed 's/^/    /' "$OUT/stubbad.log"; FAIL=$((FAIL+1))
else
  BOUT=$("$OUT/stubbad" main selftest 2>&1)
  NBAD=$(printf '%s' "$BOUT" | grep -c 'probe=BAD' || true); NBAD=${NBAD:-0}
  NOK=$(printf '%s' "$BOUT" | grep -c 'probe=OK'  || true); NOK=${NOK:-0}
  NBAD=$(printf '%s' "$NBAD" | tr -d '[:space:]'); NOK=$(printf '%s' "$NOK" | tr -d '[:space:]')
  # 2 号必须是那个"恒 OK" —— 若它也红了, 说明有人给 DEV 造了假判据
  # ⚠️ 但本臂的【灵敏度有边界】, 别把它当万能哨兵: 桩的 DEV 只有一个取值 1,
  #    所以"在 1 处成立"的判据(count>0 / count>999)它一个都抓不到,
  #    只有"把 1 判成 BAD"的判据(极性写反、无脑 return BAD)才触发。
  #    边界由 tools/hwmain_matrix_mutate.sh 的 MV8 受控 A/B 实测钉住, 非推测。
  # 🕳️🕳️ 这里必须锚到类表行, 不能只 grep '[2]': selftest 的自检行长得像
  #    "hwmain: [2] golden = 0xA57E74DF OK" 也含 [2], 首版就是这么把
  #    DEVST 取成多行的 —— 判据抓到了自己。数据本身一直是对的(8/1/DEV=OK),
  #    错的只有我的取法。教训: 判据的匹配式要比被测物更窄, 不是更宽。
  DEVST=$(printf '%s' "$BOUT" | grep '\[2\] DEV' | sed 's/.*probe=/probe=/')
  echo "  probe=BAD 计数 = $NBAD (应 = 8)   probe=OK 计数 = $NOK (应 = 1, 即 2 号 DEV)"
  echo "  2 号 DEV 实际 = $DEVST (应 = probe=OK)"
  if [ "$NBAD" = "8" ] && [ "$NOK" = "1" ] && [ "$DEVST" = "probe=OK" ]; then
    echo "  [PASS] 8 个可判别探针在契约违反时全报 BAD -> 探针确有区分力"; PASS=$((PASS+1))
  else
    echo "  [FAIL] 阴性对照不符预期 -> 有探针在契约违反时仍报 OK (= 不具区分力)"
    printf '%s' "$BOUT" | grep '\[.\]' | sed 's/^/    /'; FAIL=$((FAIL+1))
  fi
fi

echo
echo "================ 0c. 逐探针独立对照 (一次只翻一个) ================"
# 🕳️ 段 0b 只证"能让它们红", 证不了"每个探针各自接对了符号"。
#    若 main_probe_oem 错接了 hw_token_selftest, 段 0b 仍会 8 红 —— 因为
#    它照样读的是某个会坏的桩函数。本段逐个单独破坏, 要求【恰好一个】翻红
#    且翻在预期序号上: 序号错位=接错符号, 数量>1=有探针在读共享状态。
#    2 号 DEV 不在此列 —— 它无 BAD 区分力(见段 0b), 无从判别。
#    纪律: "全部一起坏"和"各自坏"是两件事, 后者才抓得出接错符号。
DISCR="0 1 3 4 5 6 7 8"
ONE_OK=1
for k in $DISCR; do
  cc -std=c11 -DSTUB_BAD_ONE=$k $INC -o "$OUT/one_$k" $SRCS 2>"$OUT/one_$k.log"
  if [ ! -x "$OUT/one_$k" ]; then
    echo "  [FAIL] break=$k 构建失败"; sed 's/^/    /' "$OUT/one_$k.log" | head -5; ONE_OK=0; FAIL=$((FAIL+1)); continue
  fi
  O=$("$OUT/one_$k" main selftest 2>&1)
  CNT=$(printf '%s' "$O" | grep -c 'probe=BAD' || true); CNT=${CNT:-0}
  CNT=$(printf '%s' "$CNT" | tr -d '[:space:]')
  AT=$(printf '%s' "$O" | grep 'probe=BAD' | sed -n 's/.*\[\([0-9]\)\].*/\1/p')
  if [ "$CNT" = "1" ] && [ "$AT" = "$k" ]; then
    printf '    · break=%s -> 恰好 1 个 BAD 且在 [%s]\n' "$k" "$AT"
  else
    printf '  [FAIL] break=%s -> BAD 数=%s 位置=[%s] (应 1 个且在 [%s])\n' "$k" "$CNT" "${AT:-无}" "$k"
    ONE_OK=0; FAIL=$((FAIL+1))
  fi
done
# 🕳️ 明细行刻意【不带】[PASS] 标记: 汇总段要拿"屏幕 [PASS] 行数"与计数器
#    对撞(见文末"三颗牙"), 任何多打一条 [PASS] 都会让对撞失败。
#    首版这里逐条打 [PASS] => 屏幕 25 / 计数 17, 自己把自己判红。
#    纪律: 明细是给人读的, [PASS]/[FAIL] 是给汇总对撞读的, 两者不要混用。
if [ "$ONE_OK" = "1" ]; then
  echo "  [PASS] 8 个探针各自独立接对符号 (无一读共享状态)"; PASS=$((PASS+1))
fi

echo
echo "================ 1. C11 vs C++17 输出 md5 逐位对拍 (HOST 模式) ================"
cc    -std=c11   -Wall -Wextra $INC -o "$OUT/hm_c11"   $SRCS 2>"$OUT/c11.log"
c++   -std=c++17 -Wall -Wextra $INC -o "$OUT/hm_cpp17" $(cpp_srcs) 2>"$OUT/cpp17.log"
w1=$(warn_count "$OUT/c11.log"); w2=$(warn_count "$OUT/cpp17.log")
echo "  C11    build 警告=$w1"
echo "  C++17  build 警告=$w2"
if [ ! -x "$OUT/hm_c11" ] || [ ! -x "$OUT/hm_cpp17" ]; then
  echo "  [FAIL] 编译失败"; sed 's/^/    /' "$OUT/c11.log" "$OUT/cpp17.log"; FAIL=$((FAIL+1))
elif [ "$w1" != "0" ] || [ "$w2" != "0" ]; then
  echo "  [FAIL] 存在警告"; grep -i warning "$OUT/c11.log" "$OUT/cpp17.log" | head; FAIL=$((FAIL+1))
else
  : > "$OUT/c11.txt"; : > "$OUT/cpp17.txt"
  for c in selftest card count sum mode; do
    "$OUT/hm_c11"   main "$c" >>"$OUT/c11.txt"   2>&1
    "$OUT/hm_cpp17" main "$c" >>"$OUT/cpp17.txt" 2>&1
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
  cc -std=c11 -Wall -Wextra $INC -DHW_MAIN_MODE_OVERRIDE=$m -c src/hw/hw_main.c -o "$OUT/ov$m.o" 2>"$OUT/ov$m.log"
  w=$(warn_count "$OUT/ov$m.log")
  if [ "$w" = "0" ]; then echo "  [PASS] mode=$m 零警告"; PASS=$((PASS+1))
  else echo "  [FAIL] mode=$m 警告 $w 条"; grep -i warning "$OUT/ov$m.log" | head; FAIL=$((FAIL+1)); fi
done

echo
echo "================ 3. freestanding 双交叉 ================"
R32="$HOME/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20230928/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"
XT="$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin/xtensa-esp-elf-gcc"
for pair in "riscv32:$R32" "xtensa:$XT"; do
  name="${pair%%:*}"; gcc="${pair#*:}"
  if [ ! -x "$gcc" ]; then echo "  [SKIP] $name 交叉编译器缺失"; continue; fi
  "$gcc" -std=c11 -Wall -Wextra -ffreestanding $INC -c src/hw/hw_main.c -o "$OUT/fs_$name.o" 2>"$OUT/fs_$name.log"
  w=$(warn_count "$OUT/fs_$name.log")
  if [ "$w" = "0" ]; then echo "  [PASS] $name freestanding 零警告"; PASS=$((PASS+1))
  else echo "  [FAIL] $name 警告 $w 条"; grep -i warning "$OUT/fs_$name.log" | head; FAIL=$((FAIL+1)); fi
done

echo
echo "================ 4. 六模式逐一 C11 vs C++17 行为对拍 ================"
for m in 0 1 2 3 4 5; do
  cc  -std=c11   -Wall -Wextra $INC -DHW_MAIN_MODE_OVERRIDE=$m -o "$OUT/m${m}_c11"   $SRCS 2>/dev/null
  c++ -std=c++17 -Wall -Wextra $INC -DHW_MAIN_MODE_OVERRIDE=$m -o "$OUT/m${m}_cpp17" $(cpp_srcs) 2>/dev/null
  if [ ! -x "$OUT/m${m}_c11" ] || [ ! -x "$OUT/m${m}_cpp17" ]; then
    echo "  [FAIL] mode=$m 编译失败"; FAIL=$((FAIL+1)); continue
  fi
  : > "$OUT/m${m}_c11.txt"; : > "$OUT/m${m}_cpp17.txt"
  for c in selftest card mode; do
    "$OUT/m${m}_c11"   main "$c" >>"$OUT/m${m}_c11.txt"   2>&1
    "$OUT/m${m}_cpp17" main "$c" >>"$OUT/m${m}_cpp17.txt" 2>&1
  done
  a=$(md5 -q "$OUT/m${m}_c11.txt"); b=$(md5 -q "$OUT/m${m}_cpp17.txt")
  if [ "$a" = "$b" ]; then echo "  [PASS] mode=$m C11 == C++17  md5=$a"; PASS=$((PASS+1))
  else echo "  [FAIL] mode=$m 不一致 $a vs $b"; diff "$OUT/m${m}_c11.txt" "$OUT/m${m}_cpp17.txt" | head -10; FAIL=$((FAIL+1)); fi
done

echo
echo "================  汇总  ================"
# 🕳️🕳️🕳️ 这个 bug 有【三颗牙】, 同一个根因: 汇总段自己的输出行只要字面含
#    [PASS]/[FAIL], 就会被任何 `grep -c` 命中 —— 于是判据把自己当成了被测物。
#
#    (下面 16/15 是【事故当时的】数字, 段数后来加到 18 —— 这几条是病史不是现规格)
#
#    牙 1 (已修): 计数器漏掉段 0 的累加 -> 屏幕 16 条 [PASS], 计数器报 15。
#    牙 2 (已修): `grep -c ... || echo 0` 在 BSD grep 零匹配时追加第二个 0,
#                  变量成了 "0\n0" -> 16==16 明明一致却报不一致。
#    牙 3 (本行修): 汇总段打印 `屏幕 [PASS] 行数 = 16`, 字面含 [PASS]。
#                  矩阵内部用 sed 截断挡住了, 但【外部读者】(mutate 脚本的
#                  run_mv) 直接 grep 整份 log, 于是把汇总行数成 FAIL ->
#                  MV3 (本该存活) 被误判成报红。
#
# ✅ 结构性修法: 汇总段不再打印字面 [PASS]/[FAIL] 标记, 改用中文字"通过/失败"。
#    这样【任何】读者 —— 矩阵自己、mutate 脚本、我本人 —— 都不会再被汇总行污染。
#    纪律: 判据的输出不能长得像被测物的输出。标记是给机器读的, 汇总是给人读的,
#    两者混用就等于让同一个词有两种含义。
SHOWN=$(grep -c '\[PASS\]' "$OUT/run.log" || true); SHOWN=${SHOWN:-0}
SHOWN_FAIL=$(grep -c '\[FAIL\]' "$OUT/run.log" || true); SHOWN_FAIL=${SHOWN_FAIL:-0}
SHOWN=$(printf '%s' "$SHOWN" | tr -d '[:space:]')
SHOWN_FAIL=$(printf '%s' "$SHOWN_FAIL" | tr -d '[:space:]')
echo "  屏幕标记行数: 通过=$SHOWN 失败=$SHOWN_FAIL   计数器: 通过=$PASS 失败=$FAIL"
if [ "$SHOWN" != "$PASS" ] || [ "$SHOWN_FAIL" != "$FAIL" ]; then
  echo "  [FAIL] 计数器与屏幕输出不一致 -> 汇总数字不作数"
  FAIL=$((FAIL+1))
  echo "  通过: $PASS   失败: $FAIL"
  echo "  有失败 ✘"; exit "$FAIL"
fi
echo "  通过: $PASS   失败: $FAIL"
[ "$FAIL" = "0" ] && echo "  全部通过 ✔" || echo "  有失败 ✘"
exit "$FAIL"
