# xiaomo 承接魔塔 int8 量化权重演示
# 用法: ./xiaomo run examples/imggen/verify_xiaomo.mo   (在 xiaomo 根目录执行)
#
# 背景: 魔塔(Modelscope) Notebook 训练出 MLP 关键字→图片生成器,
#       int8 量化后导出 imggen_quantized.json (int8 整数+scale)。
#       本脚本加载「反量化后」的 float 权重 (imggen_xiaomo.json),
#       对关键字「星空」(onehot 索引 2) 的中心像素跑一次前向,
#       验证 xiaomo 内核能正确承接魔塔量化的权重推理。
#
# 网络: x = [onehot(5) + u + v]  (7 维)
#       h  = tanh(x·W1 + b1)     (64)  | b1 偏置 (常数, 用 bias_add)
#       y  = sigmoid(h·W2 + b2)  (3=RGB)
# 输入: 星空 u=v=0.515625 的 x = [0,0,1,0,0, 0.515625, 0.515625]
# 参考(Python): RGB_out ≈ [0.05062, 0.06714, 0.06926]

# ---- 魔塔量化反量化后的 float 权重 ----
void W1 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "W1")   # 7x64
void b1 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "b1")   # 64
void W2 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "W2")   # 64x3
void b2 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "b2")   # 3

# ---- 输入: 星空关键字 (onehot 索引 2) + 中心坐标 u=v=0.515625 ----
void x : int = [0.0, 0.0, 1.0, 0.0, 0.0, 0.515625, 0.515625]

# ---- 前向: h = tanh(x·W1 + b1) ----
void z1 : int = matmul(${x}, ${W1})
void a1 : int = bias_add(${z1}, ${b1})
void h  : int = tanh(${a1})

# ---- 前向: y = sigmoid(h·W2 + b2) --- (用 W2 形状, 先 matmul 再 bias 再 sigmoid)
void z2 : int = matmul(${h}, ${W2})      # 1x3
void a2 : int = bias_add(${z2}, ${b2})   # 1x3
void y  : int = sigmoid(${a2})           # 1x3  -> RGB

>> print >> "=== 魔塔 int8 量化权重 · xiaomo 内核推理验证 (关键字=星空, 中心像素) ==="
>> print >> "-- 隐层 h (tanh, 取前 8 维) --"
void h0 : int = ${h}[0][0]
void h1 : int = ${h}[0][1]
void h2 : int = ${h}[0][2]
void h3 : int = ${h}[0][3]
void h4 : int = ${h}[0][4]
void h5 : int = ${h}[0][5]
void h6 : int = ${h}[0][6]
void h7 : int = ${h}[0][7]
>> print >> ${h0} >> " " >> ${h1} >> " " >> ${h2} >> " " >> ${h3} >> " " >> ${h4} >> " " >> ${h5} >> " " >> ${h6} >> " " >> ${h7}

>> print >> "-- 输出 RGB (sigmoid) --"
void r : int = ${y}[0][0]
void g : int = ${y}[0][1]
void b : int = ${y}[0][2]
>> print >> "R=" >> ${r}
>> print >> "G=" >> ${g}
>> print >> "B=" >> ${b}
>> print >> "参考(Python): R=0.05062 G=0.06714 B=0.06926（应一致）"
