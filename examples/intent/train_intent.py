#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
kbc 意图自动推理模型 — 训练 + Q16 导出 + .mo 生成
================================================
流程: 手写语料(6 意图) → 64 维哈希特征(与 C 宿主 infer_featurize 逐字节同款)
     → numpy 训练 2 层 MLP(64→16→6) → 权重 Q16 定点化
     → 生成 examples/intent/intent_kbc.mo (mo2kbc 可编译, while+feat() 推理)
     → 生成 intent_testset.json (e2e 对拍集)

意图表: 0=其他/chat  1=开灯  2=关灯  3=音乐  4=时间  5=天气

⚠️ 特征算法必须与 src/vm_core.c 的 infer_featurize 保持逐字节一致:
   - ASCII 字母数字连续段 = 词单元, hash("w:"+word)
   - 连续 >=0x80 字节段 = 中文运行段, 逐字节滑窗 2-gram hash + 整段 unigram hash
   - hash: h=5381; h = (h*33 + byte) mod 2^64 (FNV 变体/dfa-33); 桶 = h % 64
   - 出现即置位(Q16 presence): 特征值 ∈ {0, 65536}
"""
import json, math, os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DIM = 64
HID = 16
NCLS = 6
Q16 = 65536

# ---------------------------------------------------------------- 特征 (与 C 同款)
def featurize(text: str):
    bs = text.encode("utf-8")
    feats = set()
    word = bytearray()
    cjk = bytearray()

    def flush_word():
        if word:
            h = 5381
            for b in b"w:" + bytes(word):
                h = (h * 33 + b) % (1 << 64)
            feats.add(h % DIM)
            word.clear()

    def flush_cjk():
        if cjk:
            for k in range(len(cjk) - 1):          # 2-gram 滑窗
                h = 5381
                for b in bytes(cjk[k:k + 2]):
                    h = (h * 33 + b) % (1 << 64)
                feats.add(h % DIM)
            h = 5381                                # 整段 unigram
            for b in bytes(cjk):
                h = (h * 33 + b) % (1 << 64)
            feats.add(h % DIM)
            cjk.clear()

    for c in bs:
        if c < 0x80:
            ch = chr(c).lower().encode()[0] if chr(c).isupper() else c
            if (48 <= ch <= 57) or (65 <= ch <= 90) or (97 <= ch <= 122):
                word.append(ch)
            else:
                flush_word(); flush_cjk()
        else:
            flush_word()                            # ASCII 词被多字节打断
            cjk.append(c)
    flush_word(); flush_cjk()
    v = [0] * DIM
    for i in feats:
        v[i] = Q16
    return v

# ---------------------------------------------------------------- 语料
DATA = {
    1: ["开灯", "把灯打开", "打开客厅的灯", "帮我开一下灯", "开一下灯", "麻烦开灯",
        "灯打开", "open light", "light on", "turn on the light", "开个灯",
        "把台灯打开", "夜里把灯打开", "请打开卧室的灯", "我要开灯"],
    2: ["关灯", "把灯关掉", "关掉客厅的灯", "帮我关一下灯", "麻烦关灯", "灯关掉",
        "close light", "light off", "turn off the light", "把台灯关掉",
        "别忘了把灯关了", "关闭灯光", "睡觉前关灯", "请关掉所有灯"],
    3: ["播放音乐", "放首歌", "来点音乐", "play music", "我要听歌", "放一首歌",
        "来首歌吧", "音乐播放", "play a song", "听音乐", "给我放点歌",
        "放点轻音乐", "想听周杰伦", "来首老歌"],
    4: ["几点了", "现在几点", "查一下时间", "现在时间", "what time is it", "报时",
        "告诉我现在几点", "时间查询", "今天几号", "现在什么时间", "现在多少点了",
        "帮我看看时间", "今天星期几", "现在几点钟了"],
    5: ["今天天气怎么样", "查天气", "明天下雨吗", "weather today", "今天热不热",
        "会不会下雨", "天气预报", "查一下天气", "外面冷不冷", "明天天气",
        "今天出门要带伞吗", "周末天气如何", "现在多少度", "外面有没有下雨"],
    0: ["讲个笑话", "你是谁", "你叫什么名字", "停止", "取消", "再见", "帮帮我",
        "你好", "谢谢", "音量调大一点", "hello", "你是机器人吗", "给我讲个故事",
        "不重要", "随便聊聊"],
}
# 留出泛化验证组 (不进训练)
HOLDOUT = {
    1: ["开一下台灯", "把灯全打开"],
    2: ["灯可以关了", "把灯都关上"],
    3: ["来一首歌听听", "放音乐吧"],
    4: ["现在的时间是多少", "几点钟"],
    5: ["明天会下雨吗", "今天天气好不好"],
    0: ["今天吃什么", "你怎么这么聪明"],
}

# ---------------------------------------------------------------- 数据矩阵
def build():
    X, y, meta = [], [], []
    for lbl, texts in DATA.items():
        for t in texts:
            X.append(featurize(t)); y.append(lbl); meta.append((t, lbl, "train"))
    for lbl, texts in HOLDOUT.items():
        for t in texts:
            X.append(featurize(t)); y.append(lbl); meta.append((t, lbl, "holdout"))
    return np.array(X, dtype=np.float64) / Q16, np.array(y), meta

X, y, meta = build()
rng = np.random.default_rng(7)
W1 = rng.normal(0, 0.3, (HID, DIM)); b1 = np.zeros(HID)
W2 = rng.normal(0, 0.3, (NCLS, HID)); b2 = np.zeros(NCLS)

def fwd(Xb, W1, b1, W2, b2):
    H = np.maximum(Xb @ W1.T + b1, 0.0)
    L = H @ W2.T + b2
    return H, L

lr, epochs = 0.3, 400
Yoh = np.eye(NCLS)[y]
for ep in range(epochs):
    H, L = fwd(X, W1, b1, W2, b2)
    P = np.exp(L - L.max(1, keepdims=True)); P /= P.sum(1, keepdims=True)
    G = (P - Yoh) / len(X)
    gW2 = G.T @ H; gb2 = G.sum(0)
    GH = (G @ W2) * (H > 0)
    gW1 = GH.T @ X; gb1 = GH.sum(0)
    W1 -= lr * gW1; b1 -= lr * gb1; W2 -= lr * gW2; b2 -= lr * gb2

H, L = fwd(X, W1, b1, W2, b2)
acc = (L.argmax(1) == y).mean()
print(f"[train] 训练准确率 {acc*100:.1f}%  ({len(X)} 样本, {epochs} epochs)")

# Q16 定点 argmax 与浮点 argmax 对拍 (全样本)
W1q = np.round(W1 * Q16).astype(np.int64)
b1q = np.round(b1 * Q16 * Q16).astype(np.int64)   # b1 作用在 Q16 输出域
W2q = np.round(W2 * Q16).astype(np.int64)
b2q = np.round(b2 * Q16 * Q16).astype(np.int64)
mismatch = 0
for i in range(len(X)):
    xq = (X[i] * Q16).astype(np.int64)   # 0/65536 (build 时除过 Q16, 乘回定点域)
    hq = np.maximum(xq @ W1q.T + b1q, 0)
    lq = hq @ W2q.T + b2q
    if int(lq.argmax()) != int(L[i].argmax()):
        mismatch += 1
        print(f"  [Q16 mismatch] {meta[i][0]}: float={L[i].argmax()} q16={lq.argmax()}")
print(f"[q16] 定点/浮点 argmax 不一致: {mismatch}/{len(X)}")
assert mismatch == 0, "Q16 定点化改变了分类结果, 需要处理"
print(f"[q16] 定点范围检查: h|max|={np.abs(hq).max():.3e} (int64 安全)")

# ---------------------------------------------------------------- 生成 .mo
def arr(vals):
    return "[" + ", ".join(str(int(v)) for v in vals) + "]"

BODY = [
    "# ---- 等待宿主注入一行输入, 然后取 64 维特征 ----",
    "void rid : int = input_wait()",
    "void j : int = 0",
    "while ${j} < 64:",
    "    void fv : int = feat(${j})",
    "    x[${j}] = ${fv}",
    "    void j : int = ${j} + 1",
    "",
    "# ---- 隐层 16 (Q16 整数点积 + ReLU) ----",
    "void j2 : int = 0",
    "while ${j2} < 16:",
    "    void acc : int = 0",
    "    void i2 : int = 0",
    "    while ${i2} < 64:",
    "        void acc : int = ${acc} + x[${i2}] * W1[${j2} * 64 + ${i2}]",
    "        void i2 : int = ${i2} + 1",
    "    void acc : int = ${acc} + b1[${j2}]",
    "    if ${acc} < 0:",
    "        void acc : int = 0",
    "    h[${j2}] = ${acc}",
    "    void j2 : int = ${j2} + 1",
    "",
    "# ---- 输出层 6 (logits, Q16 域) ----",
    "void o : int = 0",
    "while ${o} < 6:",
    "    void acc2 : int = 0",
    "    void k2 : int = 0",
    "    while ${k2} < 16:",
    "        void acc2 : int = ${acc2} + h[${k2}] * W2[${o} * 16 + ${k2}]",
    "        void k2 : int = ${k2} + 1",
    "    void acc2 : int = ${acc2} + b2[${o}]",
    "    lg[${o}] = ${acc2}",
    "    void o : int = ${o} + 1",
    "",
    "# ---- argmax → 意图 id (0..5) ----",
    "void ab : int = 0",
    "void bv : int = lg[0]",
    "void t : int = 1",
    "while ${t} < 6:",
    "    void cv : int = lg[${t}]",
    "    if ${cv} > ${bv}:",
    "        void bv : int = ${cv}",
    "        void ab : int = ${t}",
    "    void t : int = ${t} + 1",
    '>> print >> "LOGITS=" >> lg[0] >> "," >> lg[1] >> "," >> lg[2] >> "," >> lg[3] >> "," >> lg[4] >> "," >> lg[5]',
    '>> print >> "INTENT=" >> ${ab}',
    "if ${ab} == 0:",
    '    >> print >> "ACTION=chat"',
    "if ${ab} == 1:",
    '    >> print >> "ACTION=light_on"',
    "if ${ab} == 2:",
    '    >> print >> "ACTION=light_off"',
    "if ${ab} == 3:",
    '    >> print >> "ACTION=play_music"',
    "if ${ab} == 4:",
    '    >> print >> "ACTION=query_time"',
    "if ${ab} == 5:",
    '    >> print >> "ACTION=query_weather"',
]

HEADER = [
    "# ============================================================",
    "# kbc 意图自动推理模型 (由 train_intent.py 生成, 勿手改)",
    "# 结构: feat(0..63) 宿主喂特征 → Q16 MLP(64→16→6, ReLU) → argmax",
    "# 权重 = numpy 真实训练导出 (examples/intent/train_intent.py)",
    "# 意图表: 0=其他 1=开灯 2=关灯 3=音乐 4=时间 5=天气",
    "",
    f"void W1 : int = {arr(W1q.reshape(-1))}",
    f"void b1 : int = {arr(b1q)}",
    f"void W2 : int = {arr(W2q.reshape(-1))}",
    f"void b2 : int = {arr(b2q)}",
    f"void x : int = {arr([0] * DIM)}",
    f"void h : int = {arr([0] * HID)}",
    f"void lg : int = {arr([0] * NCLS)}",
    "",
]

def write_mo(path, title, lines):
    out = HEADER[:5] + ["# " + title] + HEADER[5:]
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n" + "\n".join(lines) + "\n")
    print(f"[export] {path}")

# 单次版: 推理一次后自然 HALT (宿主 join 收尾, 零竞态)
write_mo(os.path.join(HERE, "intent_kbc.mo"),
         "单次版: 推理一条输入后程序结束 (text/voice/e2e 用)",
         BODY)

# 循环版: while 1 包裹, 持续监听输入 (loop 交互模式用)
loop = ["# 循环版: while 1 持续监听 (infer loop 模式)", "while 1:"]
loop += ["    " + ln if ln else "" for ln in BODY]
write_mo(os.path.join(HERE, "intent_kbc_loop.mo"), "循环版", loop)

# ---------------------------------------------------------------- 对拍集 json
ts = [{"text": t, "label": int(l), "group": g} for (t, l, g) in meta]
with open(os.path.join(HERE, "intent_testset.json"), "w") as f:
    json.dump({"dim": DIM, "hidden": HID, "ncls": NCLS, "q16": Q16, "samples": ts},
              f, ensure_ascii=False, indent=1)
print(f"[export] intent_testset.json ({len(ts)} 条)")
