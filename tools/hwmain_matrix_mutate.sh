#!/usr/bin/env bash
# ============================================================================
# hw_main 矩阵的变异验证 (V-V: 验证工具本身的验证)
#
#   纪律来源: 本轮立的规矩 —— "新写的守卫必须有变异打它"。
#   hwmain_matrix.sh 是本轮新写的判定器, 若从未被打过, 它的 15/0 无意义。
#
#   变异 4 条, 判据 = 矩阵【必须报红】; 存活也要如实报 (存活=我变异打错, 不等于守卫失效)
#
#   MV1  桩违约: parse64 吐错值, 破坏 fmt64/parse64 往返闭环
#        -> 期望: 段 0 阳性对照检出 8/9 并中止
#   MV2  摘掉桩的 extern "C" 守卫
#        -> 期望: 段 1 + 段 4 编译失败 (局部绿/全局红的经典形状)
#   MV3  形态差异写进【两形态共用】代码
#        -> 存活(正确): 共用代码两端必然相同, 证明对拍不是恒等比较
#   MV4  形态差异写进 #ifdef __cplusplus 专属分支
#        -> 期望: 段 1 + 段 4 md5 不一致
# ============================================================================
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 1

STUB=tests/hwmain_stub_probe.c
MAIN=src/hw/hw_main.c
MATRIX=tools/hwmain_matrix.sh
BK=/tmp/hwmain_mv_backup
mkdir -p "$BK"
cp "$STUB" "$BK/stub.c"
cp "$MAIN" "$BK/main.c"
cp "$MATRIX" "$BK/matrix.sh"

KILLED=0; SURVIVED=0; BROKEN=0

restore() { cp "$BK/stub.c" "$STUB"; cp "$BK/main.c" "$MAIN"; cp "$BK/matrix.sh" "$MATRIX"; }
# MV5 改的是【判定器自己】, 所以 trap 的兜底还原必须也覆盖它 ——
# 否则一旦中途异常退出, 磁盘上留下的是被打坏的矩阵, 而脚本却报告"已还原"。
trap restore EXIT

# 还原后必须复跑基线, 确认 15/0 才是真基线 (防"带着脏变异跑基线")
echo "### 基线 (还原态) ###"
bash tools/hwmain_matrix.sh >"$BK/base.log" 2>&1
BRC=$?
BASE=$(grep -c "\[PASS\]" "$BK/base.log")
echo "  rc=$BRC  PASS 条数=$BASE"
if [ "$BRC" != "0" ]; then echo "  [ABORT] 基线不绿, 变异无意义"; tail -5 "$BK/base.log"; exit 1; fi

# run_mv <名字> <期望: KILL|ALIVE> <说明> <注入函数名>
#
# 🕳️ 踩坑史 (两代, 同一个形状: 顺序错, 两代修法不同但都只治了标):
#
#   第 1 代: run_mv 开头无条件 restore() -> 刚注入的变异在跑之前被自己抹掉,
#             4 条变异全"存活"。是【注入与运行的顺序错】, 不是守卫失效。
#
#   第 2 代: 改成 "restore 只在跑完上一条后调用"(PREV_RESTORED 标记)。
#             MV2/MV3/MV4 修好了, 但 MV1 仍报"未落盘"。
#             根因 = PREV_RESTORED 初值为 0, 而首条 run_mv 进来就 restore,
#             把【调用方刚注入的 MV1】抹了 —— 治标没治本, 只是把顺序错
#             推迟到"第一条"这个特例上。
#             判别依据: 同一时刻同一文件两条口径矛盾 (脚本内读 0, 手动读 1)。
#             🕳️ 我当时把它记成"矛盾点在脚本内的执行上下文, 不在注入本身" —— 方向对,
#             但没再推一步就停手了。真相是: 不是两条口径矛盾, 是两个【时刻】,
#             手动那次在 restore 之前, 脚本那次在 restore 之后。
#
# ✅ 第 3 代 (结构性): 注入【收进 run_mv 内部】, 还原→注入→核验→运行 的顺序
#    由一个函数定死, 调用方不再有"必须在调用前注入"这个隐含契约。
#    纪律: 判据与被测物必须同源同代; 让"顺序"成为函数的内部事实, 而非
#    调用方与被调方之间的一纸口头约定 —— 口头约定迟早会漏。
run_mv() {
  local name="$1" expect="$2" desc="$3" inject="$4" scope="${5:-$STUB $MAIN}"
  restore
  "$inject"
  local injected
  # 🕳️ 旧写法 `grep -c PAT f1 f2 | awk -F:` 在 BSD grep 下每文件恰 1 处时不带文件名,
  #    awk 把整数当字段串, $2 为空 -> sum 恒 0 -> 核验永远报"未落盘"。
  #    锚点 F: 核验工具自己静默失败, 反而把真变异判成无效。
  #    修法: grep -o 逐个匹配再计数, 不依赖文件名分隔。
  #
  # 🕳️🕳️ 范围必须【恰好等于本条变异写入的文件】, 不能多也不能少:
  #    少 (MV5 改判定器自己, 却只 cat STUB/MAIN) -> 恒为 0, 误报"未落盘";
  #    多 (把 $MATRIX 也 cat 进来) -> 矩阵脚本的【注释里】写了 "MV3" 作为案例,
  #    于是 MV3 的核验恒为 2, 真注入失败时也报"已落盘" -> 判据失去区分力。
  #    纪律: 核验范围 == 变异实际写入范围, 精确相等。多一个文件都可能让判据
  #    靠一个"自己控制不了的东西"活着 —— 而那正是它最该盯的失效形态。
  injected=$(cat $scope | grep -o "$name" | wc -l | tr -d ' ')
  echo
  echo "### $name: $desc ###"
  echo "  [注入核验] 变异标记命中 $injected 处 (0 = 变异没落盘, 判据不作数)"
  if [ "$injected" = "0" ]; then
    echo "  [BROKEN ❌] 变异未落盘, 本条判据无效"
    BROKEN=$((BROKEN+1)); restore; return
  fi
  bash tools/hwmain_matrix.sh >"$BK/$name.log" 2>&1
  local rc=$?
  local p=$(grep -c "\[PASS\]" "$BK/$name.log")
  local f=$(grep -c "\[FAIL\]" "$BK/$name.log")
  echo "  rc=$rc  PASS=$p  FAIL=$f"
  # 判据: 期望杀 => 必须报红(rc!=0 或 FAIL>0); 期望活 => 必须全绿
  local red=0
  { [ "$rc" != "0" ] || [ "$f" != "0" ]; } && red=1
  if [ "$expect" = "KILL" ]; then
    if [ "$red" = "1" ]; then echo "  [KILLED ✅] $name"; KILLED=$((KILLED+1))
    else echo "  [SURVIVED ⚠️] $name 未被抓住 (守卫可能失效)"; SURVIVED=$((SURVIVED+1)); fi
  else
    if [ "$red" = "0" ]; then echo "  [ALIVED ✅ 符合预期] $name (共用代码两端同值, 对拍非恒等)"; KILLED=$((KILLED+1))
    else echo "  [BROKEN ❌] $name 期望存活却报红, 判据异常"; BROKEN=$((BROKEN+1)); fi
  fi
  restore
}

# ---- 注入函数 (各自只做一件事: 改文件 + 留标记) ----
inject_mv1() {
python3 - <<'PY'
import io
p='tests/hwmain_stub_probe.c'
s=io.open(p,encoding='utf-8').read()
old='    return v;\n}\n\n/* OEM:'
assert s.count(old)==1, 'MV1 anchor 不唯一: %d' % s.count(old)
s=s.replace(old,'    return v ^ 0x1ULL;   /* MV1 往返闭环破裂 */\n}\n\n/* OEM:')
io.open(p,'w',encoding='utf-8').write(s)
PY
}

# ---- MV1: 桩违约 (破坏往返闭环) ----
run_mv MV1 KILL "桩 parse64 吐错值 -> 往返闭环破裂" inject_mv1

# ---- MV2: 摘 extern "C" ----
inject_mv2() {
python3 - <<'PY'
import io
p='tests/hwmain_stub_probe.c'
s=io.open(p,encoding='utf-8').read()
# MV2: 摘掉守卫。留一行 MV2 注释供【注入核验】grep, 否则核验会误判未落盘
old1='#ifdef __cplusplus\nextern "C" {\n#endif\n'
old2='\n#ifdef __cplusplus\n}\n#endif\n'
assert s.count(old1)==1, 'MV2 开守卫 anchor: %d' % s.count(old1)
assert s.count(old2)==1, 'MV2 闭守卫 anchor: %d' % s.count(old2)
s=s.replace(old1,'/* MV2: guard removed */\n')
s=s.replace(old2,'')
io.open(p,'w',encoding='utf-8').write(s)
PY
}
run_mv MV2 KILL "摘掉桩的 extern C 守卫 -> C++ 侧 mangle 对不上" inject_mv2

# ---- MV3: 差异写进两形态共用代码 (期望存活) ----
inject_mv3() {
python3 - <<'PY'
import io
p='src/hw/hw_main.c'
s=io.open(p,encoding='utf-8').read()
old='    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"'
assert s.count(old)==1, 'MV3 anchor: %d' % s.count(old)
s=s.replace(old,'    "C11ONLY_HOST_MV3", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"',1)
io.open(p,'w',encoding='utf-8').write(s)
PY
}
run_mv MV3 ALIVE "差异写进两形态共用代码 -> 两端应同值(对拍非恒等)" inject_mv3

# ---- MV4: 真正的形态专属分支 ----
inject_mv4() {
python3 - <<'PY'
import io
p='src/hw/hw_main.c'
s=io.open(p,encoding='utf-8').read()
old='    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"'
assert s.count(old)==1, 'MV4 anchor: %d' % s.count(old)
new=('#ifdef __cplusplus\n'
     '    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"\n'
     '#else\n'
     '    "HOST_C11ONLY_MV4", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"\n'
     '#endif')
s=s.replace(old,new,1)
io.open(p,'w',encoding='utf-8').write(s)
PY
}
run_mv MV4 KILL "形态专属分支差异 -> 段1/段4 md5 应不一致" inject_mv4

# ---- MV5: 打【汇总自校验】本身 ----
# 🕳️ 本轮立规矩"新写的守卫必须有变异打它"。上面的"计数器 vs 屏幕 [PASS] 行数"
#    自校验就是本轮新写的守卫, 必须证明它真能报红, 否则它只是一段好看的代码。
#    手法: 摘掉段 0 的 PASS=$((PASS+1)) 累加 -> 屏幕 16 条但计数器 15 -> 应当报红。
#    判据与 MV1/MV2 的形状不同: 那两条打的是"对拍能不能发现差异",
#    MV5 打的是"汇总数字可不可信" —— 后者正是本轮新引入的东西。
inject_mv5_matrix() {
python3 - <<'PY'
import io
p='tools/hwmain_matrix.sh'
s=io.open(p,encoding='utf-8').read()
old='  echo "  [PASS] 桩使 9 个探针全 OK -> 阳性对照成立"; PASS=$((PASS+1))'
assert s.count(old)==1, 'MV5 anchor: %d' % s.count(old)
# 摘掉段 0 的累加, 模拟本轮修掉的那个真 bug 复发
s=s.replace(old,'  echo "  [PASS] 桩使 9 个探针全 OK -> 阳性对照成立"  /* MV5: 累加被摘 */')
io.open(p,'w',encoding='utf-8').write(s)
PY
}
run_mv MV5 KILL "摘掉段0的PASS累加 -> 汇总自校验必须报红" inject_mv5_matrix "$MATRIX"

echo
echo "### 还原复验 ###"
restore
bash tools/hwmain_matrix.sh >"$BK/final.log" 2>&1
FRC=$?
echo "  还原后 rc=$FRC"
diff <(grep "\[PASS\]" "$BK/base.log") <(grep "\[PASS\]" "$BK/final.log") >/dev/null \
  && echo "  还原后判据与基线逐行一致 ✔" || echo "  ⚠️ 还原后与基线不一致"
echo
echo "================  汇总  ================"
echo "  符合预期: $KILLED   存活: $SURVIVED   判据异常: $BROKEN"
[ "$SURVIVED" = "0" ] && [ "$BROKEN" = "0" ] && [ "$FRC" = "0" ] && echo "  矩阵判定器已被验证 ✔" || echo "  仍有欠账 ✘"
exit $((SURVIVED + BROKEN))
