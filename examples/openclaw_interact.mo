# =====================================================================
# openclaw_interact.mo — openclaw 交互系统 (事件循环版)
# 与 openclaw_mini.mo 的差异: 不再"写死一整天事件跑一次就 HALT",
# 而是进入无限循环: 阻塞等待用户输入 → 分类 → session 记录 → agent 处理 → 回复。
# 依赖 VM 新增 FFI: input_wait() 阻塞读一行并返回分类 cid (见 vm_core.c)。
#
# 交互模型:
#   main loop:
#     cid = input_wait()          # 阻塞直到有用户消息, 返回分类 cid
#     agent_loop(cid, T)          # session 落库 + agent 双层循环 + 回复
#     T = T + 1                    # 推进虚拟时钟
#
# 用法(验证): 宿主 ./xiaomo interact examples/openclaw_interact.mo
#   (需配套 interact 驱动的 stdin 注入, 见 src/main.c / tools)
# =====================================================================

# ---- 常量配置 ----
void MAX_MSGS : int = 24        # session 固定容量
void ROLE_USER : int = 0
void ROLE_ASST : int = 1
void ROLE_TOOL : int = 2
void CONTEXT_KEEP : int = 12    # prune 后保留条数
void MAX_TURNS : int = 5

# ---- Session 状态 (并行数组, Cache-Aside 内存层) ----
void S_roles : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_cont  : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_ts    : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_n     : int = 0
void S_leaf  : int = 0

# ---- 持久化层 P (双写) ----
void P_roles : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_cont  : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_ts    : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_n     : int = 0

# =====================================================================
# Module 1: Session
# =====================================================================
fn session_ap(role, cid, ts):
    S_roles[${S_n}] = ${role}
    S_cont[${S_n}] = ${cid}
    S_ts[${S_n}] = ${ts}
    S_leaf = ${S_n}
    S_n = ${S_n} + 1
    P_roles[${P_n}] = ${role}
    P_cont[${P_n}] = ${cid}
    P_ts[${P_n}] = ${ts}
    P_n = ${P_n} + 1
    return ${S_leaf}

fn session_cnt(): return ${S_n}

# prune: 丢弃最老消息, 保留最近 keep 条
fn session_prune(keep):
    void drop : int = ${S_n} - ${keep}
    if ${drop} > 0:
        void i : int = 0
        while ${i} < ${S_n} - ${drop}:
            S_roles[${i}] = S_roles[${i} + ${drop}]
            S_cont[${i}] = S_cont[${i} + ${drop}]
            S_ts[${i}] = S_ts[${i} + ${drop}]
            i = ${i} + 1
        S_n = ${S_n} - ${drop}
        S_leaf = ${S_leaf} - ${drop}
        >> print >> "[SESSION] prune drop=" >> ${drop} >> " now=" >> ${S_n}
    return ${S_n}

# =====================================================================
# Module 2: Agent Loop
# =====================================================================
# 交互回复: 真实 LLM 优先, 网关不可达时模板兜底
#   llm_query() → FFI 6: ESP32 TCP 连 Mac 网关(:9101) → SiliconFlow → 回复 append 到
#   前缀 "[openclaw] " 同一行; 返回 1 成功 / 0 失败(网关不可达)。
fn agent_reply(cid):
    >> print >> "[openclaw] "
    void ok : int = llm_query()
    if ${ok} == 0:
        >> print >> "（LLM 网关不可达，模板回复）"
        if ${cid} == 1:
            >> print >> "你好! 我在的, 有什么可以帮你? (可以问我: 跑工具 / 查状态)"
        if ${cid} == 2:
            >> print >> "好的, 我来调用工具处理一下... 计算完成 ✅ 结果已就绪。"
        if ${cid} == 3:
            >> print >> "当前状态: 会话消息数=" >> ${S_n} >> ", 已持久化=" >> ${P_n} >> "。一切正常运行中。"
        if ${cid} == 4:
            >> print >> "收到你的消息了(已记录到会话)。我还在成长中, 更多能力即将上线。"
    return 0

# 双层 Agent 循环 (保留 openclaw 语义: 内层 tools+steering / 外层 follow-ups)
fn agent_loop(u_cid, u_ts):
    >> print >> "[AGENT] user msg in cid=" >> ${u_cid} >> " ts=" >> ${u_ts}
    session_ap(${ROLE_USER}, ${u_cid}, ${u_ts})
    void turns : int = 0
    void tools : int = 0
    void follow_up : int = 1
    while ${follow_up} == 1:
        void has_tool : int = 1
        while ${has_tool} == 1:
            if ${turns} >= ${MAX_TURNS}:
                >> print >> "[AGENT] maxTurns reached, force stop"
                has_tool = 0
                break
            turns = ${turns} + 1
            # 简单决策: cid==2 先跑一次工具再回复; 其它直接回复
            if ${u_cid} == 2:
                if ${turns} == 1:
                    >> print >> "[AGENT] turn " >> ${turns} >> " -> execute tool"
                    session_ap(${ROLE_TOOL}, 6, ${u_ts} + ${turns})
                    tools = ${tools} + 1
                    has_tool = 1
                else:
                    >> print >> "[AGENT] turn " >> ${turns} >> " -> reply"
                    agent_reply(${u_cid})
                    has_tool = 0
            else:
                >> print >> "[AGENT] turn " >> ${turns} >> " -> reply"
                agent_reply(${u_cid})
                has_tool = 0
        follow_up = 0
    >> print >> "[AGENT] loop done turns=" >> ${turns} >> " tools=" >> ${tools}
    return ${turns}

# =====================================================================
# 主流程: 事件循环 (阻塞等输入 → 处理 → 回复), 永不 HALT
# =====================================================================
void T : int = 0

>> print >> "[BOOT] openclaw interactive running on VM. Type your message:"
session_ap(${ROLE_ASST}, 3, 0)   # boot 欢迎消息

while 1 == 1:
    void cid : int = input_wait()     # 阻塞直到有用户输入, 返回分类 cid
    T = ${T} + 1
    agent_loop(${cid}, ${T})
    # 定期 prune 控制会话窗口
    if ${S_n} > ${MAX_MSGS}:
        session_prune(${CONTEXT_KEEP})
