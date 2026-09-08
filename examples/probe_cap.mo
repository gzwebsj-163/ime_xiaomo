# probe_cap.mo — 能力边界探测: 为 openclaw-mini 下沉做语法验证
# 用法: ./xiaomo run examples/probe_cap.mo

# T1: 字符串数组
void roles : str = ["user", "assistant", "user"]
>> print >> "T1 roles[0]=" >> ${roles}[0]
>> print >> "T1 roles[1]=" >> ${roles}[1]
>> print >> "T1 roles[2]=" >> ${roles}[2]

# T2: 字符串比较 (==)
void r : str = "user"
if ${r} == "user":
    >> print >> "T2 eq=user OK"
else:
    >> print >> "T2 eq FAIL"
if ${r} != "assistant":
    >> print >> "T2 neq OK"

# T3: 函数返回数组 + 局部变量
fn make_msg(role):
    void mm : str = ${role}
    return [${mm}, "hello", 123]

void msg : str = make_msg("assistant")
>> print >> "T3 msg[0]=" >> ${msg}[0]
>> print >> "T3 msg[1]=" >> ${msg}[1]
>> print >> "T3 msg[2]=" >> ${msg}[2]

# T4: while 嵌套 + 双循环 (agent inner/outer 模拟)
void i : int = 0
while ${i} < 3:
    void j : int = 0
    while ${j} < 2:
        >> print >> "T4 outer=" >> ${i} >> " inner=" >> ${j}
        void j : int = ${j} + 1
    void i : int = ${i} + 1

# T5: 并行数组模拟 "消息对象" (role/content/ts)
void m_roles : str = ["user", "assistant", "user"]
void m_cont  : str = ["hi", "hello there", "bye"]
void m_ts    : int = [1000, 2000, 3000]
>> print >> "T5 last role=" >> ${m_roles}[2] >> " content=" >> ${m_cont}[2] >> " ts=" >> ${m_ts}[2]

# T7: 取余 / 整除
void n : int = 7
void mm : int = ${n} % 3
void dd : int = ${n} / 2
>> print >> "T7 mm=" >> ${mm} >> " dd=" >> ${dd}

# T8: 布尔逻辑 (用嵌套 if 等价实现 and)
void x : int = 5
if ${x} > 0:
    if ${x} < 10:
        >> print >> "T8 and OK"
if ${x} == 5:
    >> print >> "T8 or/eq OK"

>> print >> "PROBE_DONE"
