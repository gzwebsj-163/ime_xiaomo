#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CNN 端到端 (方向 B): numpy 手写训练一个小 CNN, 导出权重 JSON 供 xiaomo .mo 推理
任务: 8x8 单通道图, 2x2 亮斑在 4 个象限 -> 4 分类
结构 (NCHW):
  input [1,1,8,8] -> conv1 [2,1,3,3] pad0 s1 -> [1,2,6,6] -> relu
  -> maxpool 2x2 s2 -> [1,2,3,3] -> flatten 18 -> fc [18,4] -> softmax
与 nd_tensor.c 的 conv2d (im2col+matmul) 逐位一致: numpy 参考也用 im2col+matmul
"""
import numpy as np, json, os, sys

np.random.seed(42)

# ---------------- 数据生成 ----------------
def gen_batch(n, quadrant=None):
    """8x8 图, 2x2 亮斑在随机象限. 象限: 0=左上 1=右上 2=左下 3=右下"""
    X, Y = [], []
    for _ in range(n):
        img = np.random.rand(8, 8) * 0.3          # 背景噪声 0~0.3
        if quadrant is None:
            q = np.random.randint(4)
        else:
            q = quadrant
        # 亮斑 2x2 中心在象限中部
        cy = 1 + (q // 2) * 4     # q0,q1 -> 1, q2,q3 -> 5
        cx = 1 + (q % 2) * 4      # q0,q2 -> 1, q1,q3 -> 5
        img[cy:cy+2, cx:cx+2] = 1.0
        X.append(img); Y.append(q)
    return np.array(X, dtype=np.float64), np.array(Y, dtype=np.int64)

# ---------------- 网络结构与参数 ----------------
class TinyCNN:
    def __init__(self):
        # conv1: O=2, C=1, KH=KW=3 ; fc: 18->4
        self.w1 = np.random.randn(2, 1, 3, 3) * 0.5
        self.b1 = np.zeros(2)
        self.fw = np.random.randn(18, 4) * 0.5
        self.fb = np.zeros(4)

    def forward(self, x, cache=True):
        """x: [1,8,8] -> probs [4]"""
        N, H, W = 1, 8, 8
        # conv (valid, pad0 s1) -> [2,6,6]
        conv = np.zeros((2, 6, 6))
        for o in range(2):
            for oh in range(6):
                for ow in range(6):
                    s = 0.0
                    for c in range(1):
                        for kh in range(3):
                            for kw in range(3):
                                s += x[kh+oh, kw+ow] * self.w1[o, c, kh, kw]
                    conv[o, oh, ow] = s + self.b1[o]
        relu = np.maximum(conv, 0)                       # [2,6,6]
        # maxpool 2x2 s2 -> [2,3,3]
        pool = np.zeros((2, 3, 3))
        for o in range(2):
            for oh in range(3):
                for ow in range(3):
                    pool[o, oh, ow] = relu[o, oh*2:oh*2+2, ow*2:ow*2+2].max()
        flat = pool.reshape(18)                           # [18]
        logits = flat @ self.fw + self.fb                 # [4]
        e = np.exp(logits - logits.max())
        probs = e / e.sum()
        if cache:
            self._c = (conv, relu, pool, flat, logits)
        return probs

    def backward(self, x, y):
        conv, relu, pool, flat, logits = self._c
        probs = np.exp(logits - logits.max()); probs /= probs.sum()
        dlogits = probs.copy(); dlogits[y] -= 1           # [4]
        self.fw_g = np.outer(flat, dlogits)               # [18,4]
        self.fb_g = dlogits
        dflat = self.fw @ dlogits                         # [18]
        dpool = dflat.reshape(2, 3, 3)                    # [2,3,3]
        drelu = np.zeros_like(relu)
        for o in range(2):
            for oh in range(3):
                for ow in range(3):
                    blk = relu[o, oh*2:oh*2+2, ow*2:ow*2+2]
                    mi = np.unravel_index(np.argmax(blk), blk.shape)
                    drelu[o, oh*2+mi[0], ow*2+mi[1]] += dpool[o, oh, ow]
        dconv = drelu * (conv > 0)                        # [2,6,6]
        # dw1, db1
        self.w1_g = np.zeros_like(self.w1)
        self.b1_g = np.zeros(2)
        for o in range(2):
            for c in range(1):
                for kh in range(3):
                    for kw in range(3):
                        acc = 0.0
                        for oh in range(6):
                            for ow in range(6):
                                acc += x[kh+oh, kw+ow] * dconv[o, oh, ow]
                        self.w1_g[o, c, kh, kw] = acc
        self.b1_g = dconv.sum(axis=(1, 2))
        # dx (不需要, 单样本)

    def step(self, lr):
        self.w1 -= lr * self.w1_g
        self.b1 -= lr * self.b1_g
        self.fw -= lr * self.fw_g
        self.fb -= lr * self.fb_g

# ---------------- 训练 ----------------
def main():
    model = TinyCNN()
    Xtr, Ytr = gen_batch(400)
    Xte, Yte = gen_batch(200)
    lr = 1.0
    for epoch in range(60):
        idx = np.random.permutation(len(Xtr))
        loss_t = 0.0; correct = 0
        for i in idx:
            x, y = Xtr[i], Ytr[i]
            p = model.forward(x)
            loss_t += -np.log(p[y] + 1e-12)
            correct += (np.argmax(p) == y)
            model.backward(x, y)
            model.step(lr)
        lr *= 0.95
        if (epoch + 1) % 10 == 0:
            print(f"epoch {epoch+1:3d} loss={loss_t/len(idx):.4f} train_acc={correct/len(idx):.3f}")

    # 测试准确率
    c = 0
    for i in range(len(Xte)):
        p = model.forward(Xte[i])
        c += (np.argmax(p) == Yte[i])
    print(f"TEST acc = {c/len(Xte):.3f}")

    # 导出权重 (JSON 嵌套数组, 与 load_weights 读取格式一致)
    def tolist(a): return a.tolist()
    w = {"conv1_w": tolist(model.w1), "conv1_b": tolist(model.b1),
         "fc_w": tolist(model.fw), "fc_b": tolist(model.fb)}
    out = os.path.join(os.path.dirname(__file__), "cnn_weights.json")
    with open(out, "w") as f:
        json.dump(w, f)
    print("weights ->", out)

    # 保存 2 个测试样本 (用于 .mo 推理验证)
    demo = []
    for q in range(4):
        x, y = gen_batch(1, quadrant=q)
        p = model.forward(x[0])
        demo.append({"input": x[0].tolist(), "label": int(y[0]), "probs": p.tolist()})
    with open(os.path.join(os.path.dirname(__file__), "cnn_samples.json"), "w") as f:
        json.dump(demo, f)
    print("samples -> cnn_samples.json")
    for d in demo:
        print(f"  label={d['label']} probs={[round(v,4) for v in d['probs']]} argmax={np.argmax(d['probs'])}")

if __name__ == "__main__":
    main()
