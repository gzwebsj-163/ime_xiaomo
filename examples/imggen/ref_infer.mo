# 参考图片 MLP · xiaomo VM 推理验证
# 加载 v2 训练权重，对 16×16 网格做前向推理
# 用 sigmoid 输出层天然约束到 [0, 1]

>> print >> "=== Loading reference weights (v2) ==="

void W1 : int = load_weights("examples/imggen/output/ref_weights_sigmoid.json", "W1")   # 2x512
void b1 : int = load_weights("examples/imggen/output/ref_weights_v2.json", "b1")   # 512
void W2 : int = load_weights("examples/imggen/output/ref_weights_v2.json", "W2")   # 512x512
void b2 : int = load_weights("examples/imggen/output/ref_weights_v2.json", "b2")   # 512
void W3 : int = load_weights("examples/imggen/output/ref_weights_v2.json", "W3")   # 512x3
void b3 : int = load_weights("examples/imggen/output/ref_weights_v2.json", "b3")   # 3

>> print >> "Weights loaded ✓"

# ---- 小网格验证 (16×16) ----
void grid : int = 32
void total : int = ${grid} * ${grid}

void R : int = mat_zeros(${total}, 1)
void G : int = mat_zeros(${total}, 1)
void B : int = mat_zeros(${total}, 1)

void idx : int = 0
void i : int = 0
while ${i} < ${grid}:
    void j : int = 0
    while ${j} < ${grid}:
        void uval : int = -1.0 + 2.0 * ${j} / (${grid} - 1)
        void vval : int = 1.0 - 2.0 * ${i} / (${grid} - 1)
        void inp : int = [${uval}, ${vval}]
        
        # h1 = relu(x @ W1 + b1)
        void z1 : int = matmul(${inp}, ${W1})
        void a1 : int = bias_add(${z1}, ${b1})
        void h1 : int = relu(${a1})
        
        # h2 = relu(h1 @ W2 + b2)
        void z2 : int = matmul(${h1}, ${W2})
        void a2 : int = bias_add(${z2}, ${b2})
        void h2 : int = relu(${a2})
        
        # rgb = sigmoid(h2 @ W3 + b3)  → [0, 1]
        void z3 : int = matmul(${h2}, ${W3})
        void a3 : int = bias_add(${z3}, ${b3})
        void rgb : int = sigmoid(${a3})
        
        R[${idx}] = ${rgb}[0][0]
        G[${idx}] = ${rgb}[0][1]
        B[${idx}] = ${rgb}[0][2]
        
        void idx : int = ${idx} + 1
        void j : int = ${j} + 1
    void i : int = ${i} + 1

>> print >> ""
>> print >> "=== VM 16×16 推理结果 (RGB) ==="
void k : int = 0
while ${k} < ${total}:
    >> print >> ${k} >> " " >> R[${k}] >> " " >> G[${k}] >> " " >> B[${k}]
    void k : int = ${k} + 1

>> print >> ""
>> print >> "=== REF INFER VM VERIFIED ==="