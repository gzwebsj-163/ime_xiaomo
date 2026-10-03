# -*- coding: utf-8 -*-
"""
zh_rnn_gen.py — zh_rnn_step 自回归生成驱动器 (服务器端)
权重/词表/bc 在 _vm_demo 目录。每次调用不再重编译:
  - load(): 加载 bc + 权重 + 词表 (lru 缓存)
  - step(x_idx, h): 单步前向, 返回 (prob[V], h_next[H])
  - sample(prob, temperature, top_k): token 采样 (greedy / multinomial)
  - generate(...): 自回归循环 (EOS 终止 + max_len 兜底)

EOS 约定: 词表索引 0 = '。' (句号) 视为 EOS。
"""
import os
import json
import random
import numpy as np

BC_DIR = "/app/_vm_demo"
BC_NAME = "zh_rnn_step.bc"

_CACHE = {}


def _import_vm(force=False):
    global _CACHE
    if force or "vm" not in _CACHE:
        import sys
        for p in ("/app", "/app/app/channel/web"):
            if p not in sys.path:
                sys.path.insert(0, p)
        from crypto.obf_engine.layers.layer5_private_ir.bytecode import ByteCode
        from crypto.obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter
        _CACHE["vm"] = (ByteCode, VMInterpreter)
    return _CACHE["vm"]


def _load(force=False):
    """加载并缓存 bc/权重/词表。返回 dict。"""
    global _CACHE
    if force or "model" not in _CACHE:
        ByteCode, VMInterpreter = _import_vm()
        data = open(os.path.join(BC_DIR, BC_NAME), "rb").read()
        bc = None
        for d in (True, False):
            try:
                bc = ByteCode.deserialize(data, decrypt=d)
                break
            except Exception:
                continue
        if bc is None:
            raise RuntimeError("cannot parse %s" % BC_NAME)
        w = np.load(os.path.join(BC_DIR, "zh_rnn_weights.npz"))
        vocab = json.load(open(os.path.join(BC_DIR, "zh_vocab.json"), encoding="utf-8"))["vocab"]
        model = {
            "bc": bc,
            "W_in": w["W_in"], "W_h": w["W_h"], "b_h": w["b_h"],
            "W_out": w["W_out"], "b_out": w["b_out"],
            "vocab": vocab,
            "V": len(vocab), "H": w["W_h"].shape[0],
            "VMInterpreter": VMInterpreter, "ByteCode": ByteCode,
        }
        _CACHE["model"] = model
    return _CACHE["model"]


def step(x_idx, h=None):
    """单步前向。x_idx: 词表索引(int); h: (H,) 上一隐藏状态或 None(=0)。
    返回 (prob: np.ndarray[V], h_next: np.ndarray[H])。"""
    m = _load()
    V, H = m["V"], m["H"]
    if h is None:
        h = np.zeros(H)
    x = np.zeros(V)
    x[x_idx] = 1.0
    args = [x.tolist(), np.asarray(h, dtype=float).ravel().tolist(),
            m["W_in"].tolist(), m["W_h"].tolist(), m["b_h"].tolist(),
            m["W_out"].tolist(), m["b_out"].tolist()]
    vm = m["VMInterpreter"](max_instructions=2_000_000)
    vm.load_bytecode(m["bc"])
    out = np.asarray(vm.call(*args), dtype=float).ravel()
    prob = out[:V]
    h_next = out[V:V + H]
    return prob, h_next


def sample(prob, temperature=1.0, top_k=0):
    """按分布采样 token 索引。
    - temperature: logits 缩放 (temperature->0 趋近贪心)
    - top_k: 0=全部; 否则只在 top-k 内采样
    mode 由调用方选择: sample(prob, ..., greedy=True) 或走概率采样。
    """
    p = np.asarray(prob, dtype=float).ravel()
    eps = 1e-12
    p = np.clip(p, eps, None)
    if top_k > 0:
        k = min(top_k, len(p))
        inds = np.argpartition(p, -k)[-k:]
        mask = np.zeros_like(p, dtype=bool)
        mask[inds] = True
        p = np.where(mask, p, 0.0)
    p = p / p.sum()
    # temperature 采样(多项式)
    if temperature <= 0:
        return int(np.argmax(prob))
    # Gumbel-softmax 式采样: 用重参数化保证可重现代入
    r = -np.log(-np.log(np.random.random_sample(len(p)) + 1e-9) + 1e-9)
    logp = np.log(p + 1e-12) / max(temperature, 1e-9)
    return int(np.argmax(logp + r))


def generate(start, max_len=48, mode="greedy", temperature=1.0, top_k=0,
             eos_idx=0, h0=None, seed_state=None, step_cb=None):
    """自回归生成。
    start: 起始字符(str)或索引(int); 若为 str 取首个字符。
    mode: 'greedy'(argmax) 或 'sample'(多项式采样, 用 temperature/top_k)。
    eos_idx: EOS token 索引(默认0='。')。
    h0: 初始隐藏状态(可选)。
    seed_state: 可选随机种子(int)保证可复现。
    step_cb: 每步回调 (step_no, idx, prob) 便于记录概率。
    返回 dict: {tokens, text, probs, states, logprobs, finished, hits, reason}
    """
    m = _load()
    vocab, V, H = m["vocab"], m["V"], m["H"]
    if isinstance(start, str):
        start = start[0]
        if start not in vocab:
            raise ValueError("起始字符不在词表: %r" % start)
        idx0 = vocab.index(start)
    else:
        idx0 = int(start)
        idx0 = idx0 % V
    if seed_state is not None:
        np.random.seed(seed_state)
        random.seed(seed_state)

    h = np.zeros(H) if h0 is None else np.asarray(h0, dtype=float).ravel()
    tokens = [idx0]
    probs = []
    states = []
    logprobs = []
    finished = False
    reason = "max_len"
    cur = idx0
    for n in range(1, max_len + 1):
        prob, h_next = step(cur, h)
        probs.append(prob)
        states.append(h.copy())
        logp = float(np.log(np.clip(prob[cur], 1e-12, None)).item()) if n == 1 else None
        if mode == "greedy":
            nxt = int(np.argmax(prob))
        else:
            nxt = sample(prob, temperature=temperature, top_k=top_k)
        if step_cb:
            step_cb(n, nxt, prob)
        tokens.append(nxt)
        if n == 1:
            logprobs.append(logp)
        pred = vocab[nxt] if nxt < V else "?"
        if nxt == eos_idx:
            finished = True
            reason = "eos"
            break
        cur = nxt
        h = h_next
    text = "".join(vocab[t] if t < V else "?" for t in tokens)
    return {
        "tokens": tokens,
        "text": text,
        "probs": probs,
        "states": states,
        "finished": finished,
        "reason": reason,
        "hit_eos": finished,
    }
