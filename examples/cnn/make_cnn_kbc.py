#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_cnn_kbc.py — 方向 A：把 CNN 推理下沉 Kills 字节码 (.kbc)
把 cnn_demo.json 的 float 权重/输入 定点化(Q16) → 生成一维数组 + WHILE 展开的
cnn_kbc.mo → mo2kbc 编译成 .kbc → 四端跑同一字节码对拍。
零 C 改动：利用现有 .kbc 整数机 + 定点数模拟 float CNN。

⚠️ 重要：conv 用【训练 TinyCNN 语义】(非标准互相关)：
    conv = col @ w1.reshape(2,9).T + b1   # [36,2]
    conv = conv.reshape(1,2,6,6)          # row-major 错位布局
 即输出位置 (oc,i,j) 的扁平 loc=oc*36+i*6+j，取 窗口 win=loc//2, 通道 ch=loc%2。
 训练/解释器/.kbc 三端语义必须一致，argmax 才能 = label=3。
"""
import json, os
import numpy as np

Q = 1 << 16  # Q16 定点

BASE = os.path.dirname(os.path.abspath(__file__))
JSON = os.path.join(BASE, "cnn_demo.json")
MO   = os.path.join(BASE, "cnn_kbc.mo")
KBC  = os.path.join(BASE, "cnn_kbc.kbc")

def tinycnn_forward(x, w1, b1, fw, fb):
    """x:[1,8,8] -> 训练语义 logits + probs"""
    N = x.shape[0]
    col = np.empty((N, 6, 6, 3, 3))
    for i in range(3):
        for j in range(3):
            col[:, :, :, i, j] = x[:, i:i+6, j:j+6]
    col = col.reshape(N*36, 9)
    conv = col @ w1.reshape(2, 9).T + b1          # [36N,2]
    conv = conv.reshape(N, 2, 6, 6)               # 错位布局（训练语义）
    relu = np.maximum(conv, 0)
    pool = relu.reshape(N, 2, 3, 2, 3, 2).max(axis=(3, 5))
    flat = pool.reshape(N, 18)
    logits = flat @ fw + fb
    e = np.exp(logits - logits.max(axis=1, keepdims=True))
    probs = e / e.sum(axis=1, keepdims=True)
    return logits, probs

def main():
    d = json.load(open(JSON))
    x   = np.array(d["input"], dtype=np.float64).reshape(1, 8, 8)
    w1  = np.array(d["conv1_w"], dtype=np.float64)   # [2,1,3,3]
    b1  = np.array(d["conv1_b"], dtype=np.float64)   # [2]
    fw  = np.array(d["fc_w"], dtype=np.float64)      # [18,4]
    fb  = np.array(d["fc_b"], dtype=np.float64)      # [4]
    label = int(d["label"])

    # ---- float 参考（训练语义）----
    lg_f, p_f = tinycnn_forward(x, w1[:, 0], b1, fw, fb)
    ref_argmax = int(np.argmax(lg_f[0]))
    print(f"[float 参考] logits={np.round(lg_f[0],4)} argmax={ref_argmax} label={label}")
    assert ref_argmax == label, f"float argmax({ref_argmax}) != label({label})"

    # ---- 定点化 Q16 ----
    q  = lambda a: np.round(a * Q).astype(np.int64)
    qx  = q(x.reshape(64))
    qw1 = q(w1[:, 0].reshape(18))   # [oc*9 + kh*3 + kw]
    qb1 = q(b1.reshape(2))
    qfw = q(fw.reshape(72))
    qfb = q(fb.reshape(4))

    # ---- 定点 numpy 验证（错位语义）: 输出扁平 c[0,2,6,6] row-major ----
    qc = np.zeros(72, dtype=np.int64)
    for oc in range(2):
        for i in range(6):
            for j in range(6):
                loc = oc*36 + i*6 + j
                win, ch = loc//2, loc%2
                oh, ow = win//6, win%6
                acc = 0
                for kh in range(3):
                    for kw in range(3):
                        acc += qx[(oh+kh)*8 + (ow+kw)] * qw1[ch*9 + kh*3 + kw]
                acc += qb1[ch]
                if acc < 0: acc = 0
                qc[loc] = acc
    qp = np.zeros(18, dtype=np.int64)
    for oc in range(2):
        for oh in range(3):
            for ow in range(3):
                v = qc[oc*36+(2*oh)*6+2*ow]
                for (dh,dw) in [(0,1),(1,0),(1,1)]:
                    v2 = qc[oc*36+(2*oh+dh)*6+2*ow+dw]
                    if v2 > v: v = v2
                qp[oc*9+oh*3+ow] = v
    qlg = np.zeros(4, dtype=np.int64)
    for out in range(4):
        acc = qfb[out]
        for k in range(18):
            acc += qp[k] * qfw[k*4+out]
        qlg[out] = acc
    q_argmax = int(np.argmax(qlg))
    print(f"[定点参考] logits={qlg.tolist()} argmax={q_argmax}")
    assert q_argmax == label, f"定点 argmax({q_argmax}) != label({label})"
    print("✅ 定点分类与 float/label 一致")

    # ---- 生成 .mo ----
    def arr1d(a):
        return "[" + ", ".join(str(int(v)) for v in a) + "]"

    lines = []
    A = lines.append
    A("# 由 make_cnn_kbc.py 生成: CNN 定点(Q16)推理, 一维数组 + WHILE 展开")
    A("# 语义 = 训练 TinyCNN (conv reshape 错位布局), argmax 分类")
    A("")
    A("void x : int = " + arr1d(qx))
    A("void w1 : int = " + arr1d(qw1))
    A("void b1 : int = " + arr1d(qb1))
    A("void fw : int = " + arr1d(qfw))
    A("void fb : int = " + arr1d(qfb))
    A("void c : int = " + arr1d(np.zeros(72, dtype=np.int64)))
    A("void p : int = " + arr1d(np.zeros(18, dtype=np.int64)))
    A("void lg : int = " + arr1d(np.zeros(4, dtype=np.int64)))
    A("")
    A("# ---- CONV1 (训练语义: loc=oc*36+i*6+j, win=loc/2, ch=loc%2) ----")
    A("void oc : int = 0")
    A("while ${oc} < 2:")
    A("    void oh : int = 0")
    A("    while ${oh} < 6:")
    A("        void ow : int = 0")
    A("        while ${ow} < 6:")
    A("            void loc : int = ${oc} * 36 + ${oh} * 6 + ${ow}")
    A("            void win : int = ${loc} / 2")
    A("            void ch : int = ${loc} - ${win} * 2")
    A("            void wh : int = ${win} / 6")
    A("            void ww : int = ${win} - ${wh} * 6")
    A("            void acc : int = 0")
    A("            void kh : int = 0")
    A("            while ${kh} < 3:")
    A("                void kw : int = 0")
    A("                while ${kw} < 3:")
    A("                    void acc : int = ${acc} + x[${wh} * 8 + ${ww} + ${kh} * 8 + ${kw}] * w1[${ch} * 9 + ${kh} * 3 + ${kw}]")
    A("                    void kw : int = ${kw} + 1")
    A("                void kh : int = ${kh} + 1")
    A("            void acc : int = ${acc} + b1[${ch}]")
    A("            if ${acc} < 0:")
    A("                void acc : int = 0")
    A("            c[${loc}] = ${acc}")
    A("            void ow : int = ${ow} + 1")
    A("        void oh : int = ${oh} + 1")
    A("    void oc : int = ${oc} + 1")
    A("")
    A("# ---- MAXPOOL 2x2 s2 (c 为标准布局) ----")
    A("void oc : int = 0")
    A("while ${oc} < 2:")
    A("    void oh : int = 0")
    A("    while ${oh} < 3:")
    A("        void ow : int = 0")
    A("        while ${ow} < 3:")
    A("            void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2]")
    A("            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 1]:")
    A("                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 1]")
    A("            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 6]:")
    A("                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 6]")
    A("            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 7]:")
    A("                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 7]")
    A("            p[${oc} * 9 + ${oh} * 3 + ${ow}] = ${ma}")
    A("            void ow : int = ${ow} + 1")
    A("        void oh : int = ${oh} + 1")
    A("    void oc : int = ${oc} + 1")
    A("")
    A("# ---- FC (18x4) ----")
    A("void fout : int = 0")
    A("while ${fout} < 4:")
    A("    void facc : int = fb[${fout}]")
    A("    void fk : int = 0")
    A("    while ${fk} < 18:")
    A("        void facc : int = ${facc} + p[${fk}] * fw[${fk} * 4 + ${fout}]")
    A("        void fk : int = ${fk} + 1")
    A("    lg[${fout}] = ${facc}")
    A("    void fout : int = ${fout} + 1")
    A("")
    A("# ---- ARGMAX ----")
    A("void ai : int = 1")
    A("void av : int = lg[0]")
    A("void ab : int = 0")
    A("while ${ai} < 4:")
    A("    if ${av} < lg[${ai}]:")
    A("        void av : int = lg[${ai}]")
    A("        void ab : int = ${ai}")
    A("    void ai : int = ${ai} + 1")
    A("")
    A(">> print >> \"=== KBC CNN 定点推理 (Q16) ===\"")
    A(">> print >> \"logits=\" >> lg[0] >> \",\" >> lg[1] >> \",\" >> lg[2] >> \",\" >> lg[3]")
    A(">> print >> \"argmax=\" >> ${ab}")

    open(MO, "w").write("\n".join(lines) + "\n")
    print(f"✅ 已生成 {MO} ({os.path.getsize(MO)} bytes)")
    print(f"   期望 logits(Q16)={qlg.tolist()} argmax={q_argmax}")

if __name__ == "__main__":
    main()
