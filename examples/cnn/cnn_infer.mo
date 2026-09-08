# ============================================================
# xiaomo CNN 端到端推理 (方向 B)
# 结构: input[1,1,8,8] -> conv1[2,1,3,3] pad0 s1 -> [1,2,6,6]
#       -> relu -> maxpool 2x2 s2 -> [1,2,3,3]
#       -> flatten [1,18] -> fc[18,4] -> softmax -> 4 分类
# 权重来自 numpy 训练 (cnn_demo.json), 与 numpy 参考逐位对拍
# 用法: ./xiaomo run examples/cnn/cnn_infer.mo
# ============================================================

>> print >> "=== xiaomo CNN 前向推理 (四象限 4 分类) ==="

# ---- 加载权重与输入 (全部来自 cnn_demo.json) ----
void x  : int = load_weights("examples/cnn/cnn_demo.json", "input")   # [1,1,8,8]
void w1 : int = load_weights("examples/cnn/cnn_demo.json", "conv1_w") # [2,1,3,3]
void b1 : int = load_weights("examples/cnn/cnn_demo.json", "conv1_b") # [2]
void fw : int = load_weights("examples/cnn/cnn_demo.json", "fc_w")    # [18,4]
void fb : int = load_weights("examples/cnn/cnn_demo.json", "fc_b")    # [4]

# ---- 卷积 + 偏置 + relu ----
void c : int = conv2d(${x}, ${w1}, 0, 0, 1, 1)        # [1,2,6,6]
void b1r : int = reshape(${b1}, [2, 1, 1])            # bias 对齐通道轴
void c1 : int = nd_add(${c}, ${b1r})                  # [1,2,6,6] + [2,1,1]
void r : int = nd_relu(${c1})                         # [1,2,6,6]

# ---- 最大池化 ----
void p : int = maxpool2d(${r}, 2, 2, 0, 0, 2, 2)      # [1,2,3,3]

# ---- 展平 + 全连接 + softmax ----
void f : int = reshape(${p}, [1, 18])                 # [1,18]
void lo : int = nd_matmul(${f}, ${fw})                # [1,4]
void lg : int = nd_add(${lo}, ${fb})                  # 广播 + bias [4]
void pr : int = nd_softmax(${lg}, -1)                 # [1,4]

# ---- 输出 ----
>> print >> "-- 输入 x [1,1,8,8] --"
>> print >> ${x}
>> print >> "-- 卷积输出 c [1,2,6,6] --"
>> print >> ${c1}
>> print >> "-- relu 输出 r --"
>> print >> ${r}
>> print >> "-- 池化输出 p [1,2,3,3] --"
>> print >> ${p}
>> print >> "-- 展平 f [1,18] --"
>> print >> ${f}
>> print >> "-- logits (fc+bias) --"
>> print >> ${lg}
>> print >> "-- softmax 概率 --"
>> print >> ${pr}

>> print >> "=== CNN 推理完成 ==="
