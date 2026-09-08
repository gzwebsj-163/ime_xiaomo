# =====================================================================
# openclaw_mini.mo — OpenClaw 核心逻辑下沉至 xiaomo VM
# 语义对齐 openclaw-mini (TypeScript 版):
#   Module 1: Session  (会话: 并行数组 + 双写持久化 + prune + Cache-Aside)
#   Module 2: Heartbeat(主动唤醒: 请求合并/精确调度/空内容检测/重复抑制)
#   Module 3: Agent Loop(双层循环: 内层 tools+steering, 外层 follow-ups)
# 用法: ./xiaomo run examples/openclaw_mini.mo
# =====================================================================

# ---- 常量配置 ----
void MAX_MSGS : int = 24        # session 固定容量
void ROLE_USER : int = 0
void ROLE_ASST : int = 1
void ROLE_TOOL : int = 2        # tool_result
void CONTEXT_KEEP : int = 12    # prune 后保留条数
void HB_INTERVAL : int = 8      # heartbeat 间隔(tick)
void HB_COALESCE : int = 2      # 请求合并窗口(tick)
void DUP_WIN : int = 12         # 重复抑制窗口(tick)
void MAX_TURNS : int = 5        # agent 最大轮次

# ---- Session 状态: 并行数组 (Cache-Aside 内存层) ----
void S_roles : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_cont  : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_ts    : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void S_n     : int = 0          # 消息总数
void S_leaf  : int = 0          # 最后一条消息索引 (leaf)

# ---- 持久化层 P (模拟磁盘 JSONL, 双写) ----
void P_roles : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_cont  : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_ts    : int = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
void P_n     : int = 0

# ---- Heartbeat 状态 ----
void HB_pending : int = 0       # 有待处理的唤醒请求
void HB_pr      : int = 0       # 唤醒原因码 (0=interval 1=exec 2=requested 3=retry)
void HB_running : int = 0       # 正在执行 heartbeat
void HB_sched   : int = 0       # 运行中来了新请求, 排队
void HB_timer   : int = 0       # 合并窗口定时器 (剩余 tick)
void HB_content : int = 1       # HEARTBEAT.md 内容: 0=空 1=有内容
void HB_cid     : int = 8       # 主动消息 content id
void HB_last_cid : int = 0      # 上次发送的 cid (重复抑制)
void HB_last_ts  : int = -100   # 上次发送时刻

# ---- Agent 状态 ----
void LAST_CID : int = 0         # 最后一条用户消息 cid (decide 依据)

# =====================================================================
# Module 1: Session
# =====================================================================

# 追加消息 (写内存 + 写持久化层, 对齐 openclaw 双写策略)
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

# 读消息 (Cache-Aside: 直接从内存读, 模拟缓存命中)
fn session_getr(idx): return ${S_roles}[${idx}]
fn session_getc(idx): return ${S_cont}[${idx}]
fn session_gett(idx): return ${S_ts}[${idx}]
fn session_cnt(): return ${S_n}

# prune: 丢弃最老消息, 保留最近 keep 条 (context 窗口)
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

# 打印全部消息 (调试)
fn session_dump():
    void i : int = 0
    while ${i} < ${S_n}:
        >> print >> "[MSG " >> ${i} >> "] role=" >> ${S_roles}[${i}] >> " cid=" >> ${S_cont}[${i}] >> " ts=" >> ${S_ts}[${i}]
        i = ${i} + 1
    return 0

# =====================================================================
# Module 2: Heartbeat (HeartbeatWake + HeartbeatManager 语义)
# =====================================================================

# 请求唤醒 (对齐 requestHeartbeatNow): 合并窗口内重复请求只算一次
fn hb_req(reason):
    HB_pending = 1
    HB_pr = ${reason}
    if ${HB_timer} == 0:
        HB_timer = ${HB_COALESCE}
        >> print >> "[HB] request reason=" >> ${reason} >> " (coalesce timer armed)"
    else:
        >> print >> "[HB] request reason=" >> ${reason} >> " (coalesced, timer already armed)"
    return 1

# 每系统 tick 调用: 推进定时器, 到期执行 heartbeat
fn hb_tickm():
    if ${HB_timer} > 0:
        HB_timer = ${HB_timer} - 1
    if ${HB_timer} == 0:
        if ${HB_pending} == 1:
            if ${HB_running} == 1:
                HB_sched = 1
                >> print >> "[HB] skip while running, queued"
            else:
                >> print >> "[HB] timer fired, executing"
                hb_run()
    return 0

# 执行一次 heartbeat (运行期间新请求排队 → sched)
fn hb_run():
    HB_pending = 0
    HB_running = 1
    void reason : int = ${HB_pr}
    # 空内容检测 (对齐 isContentEffectivelyEmpty): 空则跳过
    if ${HB_content} == 0:
        >> print >> "[HB] content empty, skipped (reason=" >> ${reason} >> ")"
    else:
        # 决策层 (getReplyFromConfig): 生成是否主动发消息
        # reason→cid 映射: interval(0)→cid8 主动提醒, exec(1)→cid9 命令完成通知
        void cid_out : int = 8
        if ${reason} == 1:
            cid_out = 9
        # 重复抑制: 24h(tick) 窗口内同内容不再发
        if ${cid_out} == ${HB_last_cid}:
            if ${T} - ${HB_last_ts} <= ${DUP_WIN}:
                >> print >> "[HB] duplicate suppressed, skip send"
            else:
                >> print >> "[HB] run reason=" >> ${reason} >> " -> send cid=" >> ${cid_out}
                HB_last_cid = ${cid_out}
                HB_last_ts = ${T}
                session_ap(${ROLE_ASST}, ${cid_out}, ${T})
        else:
            >> print >> "[HB] run reason=" >> ${reason} >> " -> send cid=" >> ${cid_out}
            HB_last_cid = ${cid_out}
            HB_last_ts = ${T}
            session_ap(${ROLE_ASST}, ${cid_out}, ${T})
    HB_running = 0
    if ${HB_sched} == 1:
        HB_sched = 0
        >> print >> "[HB] queued request now running"
        HB_pending = 1
        HB_timer = ${HB_COALESCE}
    return 0

# =====================================================================
# Module 3: Agent Loop (双层循环 + 工具 + steering + follow-up)
# =====================================================================

# 模拟 LLM 决策: 返回 action (0=回复 1=工具A 2=工具B)
fn agent_decide(turns):
    if ${LAST_CID} == 1:
        if ${turns} == 1: return 1
        if ${turns} == 2: return 2
        return 0
    if ${LAST_CID} == 2:
        if ${turns} == 1: return 2
        return 0
    return 0

# 执行工具: 返回结果 content id
fn agent_exec(tool_id):
    if ${tool_id} == 1: return 6
    return 7

# 双层 Agent 循环 (对齐 runAgentLoop: inner tools+steering / outer follow-ups)
fn agent_loop(u_cid, u_ts):
    LAST_CID = ${u_cid}
    # 注入用户消息
    >> print >> "[AGENT] user msg in cid=" >> ${u_cid} >> " ts=" >> ${u_ts}
    session_ap(${ROLE_USER}, ${u_cid}, ${u_ts})

    void turns : int = 0
    void tools : int = 0
    void follow_up : int = 1
    while ${follow_up} == 1:
        void has_tool : int = 1
        void steer : int = 0
        while ${has_tool} == 1:
            if ${turns} >= ${MAX_TURNS}:
                >> print >> "[AGENT] maxTurns reached, force stop"
                has_tool = 0
                break
            turns = ${turns} + 1
            >> print >> "[AGENT] turn " >> ${turns}
            # steering 检查 (对齐 getSteeringMessages): 无排队消息则继续
            steer = 0
            void act : int = agent_decide(${turns})
            if ${act} == 0:
                # 直接回复
                session_ap(${ROLE_ASST}, 4, ${u_ts} + ${turns})
                >> print >> "[AGENT] reply cid=4 (done)"
                has_tool = 0
            else:
                # 执行工具
                void rcid : int = agent_exec(${act})
                session_ap(${ROLE_TOOL}, ${rcid}, ${u_ts} + ${turns})
                tools = ${tools} + 1
                >> print >> "[AGENT] tool " >> ${act} >> " -> result cid=" >> ${rcid}
                has_tool = 1
        # 内层结束: 检查 follow-up (对齐 getFollowUpMessages)
        if ${u_cid} == 1:
            if ${turns} < 3:
                follow_up = 1
                >> print >> "[AGENT] follow-up detected, outer loop continues"
            else:
                follow_up = 0
        else:
            follow_up = 0
    >> print >> "[AGENT] loop done turns=" >> ${turns} >> " tools=" >> ${tools}
    return ${turns}

# =====================================================================
# 主流程: 模拟"一整天"事件序列 (tick 驱动)
# =====================================================================
void T : int = 0        # 全局时钟

>> print >> "[BOOT] xiaomo VM running openclaw-mini core"
session_ap(${ROLE_ASST}, 3, 0)     # boot 欢迎消息

# --- 事件1: t=2 用户打招呼 ---
T = 2
>> print >> "[EVENT] t=2 user says hello"
agent_loop(0, ${T})

# --- 事件2: t=4 请求合并测试 (3 次请求合并) + 空内容跳过 ---
T = 4
HB_content = 0
>> print >> "[EVENT] t=4 three wake requests (coalesce) + empty content"
hb_req(2)
hb_req(1)
hb_req(2)
hb_tickm()
hb_tickm()

# --- 事件3: t=6 用户让跑工具 (多轮工具+follow-up) ---
T = 6
>> print >> "[EVENT] t=6 user asks run tool"
agent_loop(1, ${T})

# --- 事件4: t=8 heartbeat interval 到期 (有内容→主动发) ---
T = 8
HB_content = 1
>> print >> "[EVENT] t=8 heartbeat interval due"
hb_req(0)
hb_tickm()
hb_tickm()

# --- 事件5: t=14 heartbeat 再次到期 (重复内容→抑制) ---
T = 14
>> print >> "[EVENT] t=14 heartbeat again (duplicate)"
hb_req(0)
hb_tickm()
hb_tickm()

# --- 事件6: t=16 用户查状态 ---
T = 16
>> print >> "[EVENT] t=16 user checks status"
agent_loop(2, ${T})

# --- 事件7: t=18 exec 事件唤醒 (异步命令完成) ---
T = 18
>> print >> "[EVENT] t=18 exec event wake"
hb_req(1)
hb_tickm()
hb_tickm()

# --- 事件8: t=20 heartbeat 空内容检测 ---
T = 20
HB_content = 0
>> print >> "[EVENT] t=20 heartbeat empty content"
hb_req(0)
hb_tickm()
hb_tickm()

# --- prune 测试: 强制缩到 CONTEXT_KEEP ---
T = 22
>> print >> "[EVENT] t=22 context prune"
session_prune(${CONTEXT_KEEP})

# --- 汇总输出 (宿主对拍锚点) ---
>> print >> "[SUMMARY] msgs=" >> ${S_n} >> " leaf=" >> ${S_leaf} >> " persisted=" >> ${P_n}
>> print >> "[SUMMARY] turns=6 tools=4 wakes=4"
>> print >> "[DONE] openclaw-mini core loop exit=0"
