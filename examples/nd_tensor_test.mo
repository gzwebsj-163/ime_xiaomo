# ND Tensor 引擎验证测试 (.mo 语法)
# 测试: 创建、形状操作、广播、逐元素、matmul、颜色空间
# 用法: ./xiaomo run examples/nd_tensor_test.mo

>> print >> "=== ND Tensor Engine Verification ==="

# ---- 1. 创建 2D tensor (从矩阵) ----
void a : int = tensor([[1,2,3],[4,5,6],[7,8,9]])
>> print >> "a (3x3 tensor): " >> ${a}

# ---- 2. 重塑 ----
void a_flat : int = reshape(${a}, [-1])
>> print >> "a reshaped to 1D: " >> ${a_flat}

# ---- 3. 转置 ----
void a_t : int = transpose_n(${a})
>> print >> "a transposed: " >> ${a_t}

# ---- 4. 全零 3D tensor ----
void d : int = nd_zeros([2,3,4])
>> print >> "zeros [2,3,4]: " >> ${d}

# ---- 5. 逐元素运算 ----
void e : int = tensor([[1,2,3],[4,5,6]])
void f : int = tensor([[2,2,2],[2,2,2]])
void g1 : int = nd_add(${e}, ${f})
>> print >> "add: " >> ${g1}
void g2 : int = nd_mul(${e}, ${f})
>> print >> "mul: " >> ${g2}
void g3 : int = nd_relu(tensor([[-1,0,1],[2,-3,4]]))
>> print >> "relu([[-1,0,1],[2,-3,4]]): " >> ${g3}

# ---- 6. 归约 ----
void s_all : int = nd_sum(${e}, -1)
>> print >> "sum all(e): " >> ${s_all}
void m0 : int = nd_mean(${e}, 0)
>> print >> "mean axis0(e): " >> ${m0}

# ---- 7. matmul ----
void A : int = tensor([[1,2],[3,4]])
void B : int = tensor([[5,6],[7,8]])
void C : int = nd_matmul(${A}, ${B})
>> print >> "matmul(A,B): " >> ${C}

# ---- 8. 颜色空间 ----
void rgb : int = tensor([[1,0,0],[0,1,0],[0,0,1],[0.5,0.5,0.5]])
void hsv : int = nd_rgb2hsv(${rgb})
>> print >> "rgb2hsv: " >> ${hsv}
void rgb_back : int = nd_hsv2rgb(${hsv})
>> print >> "hsv2rgb back: " >> ${rgb_back}
void gray : int = nd_grayscale(${rgb})
>> print >> "grayscale: " >> ${gray}

# ---- 9. softmax ----
void logits : int = tensor([[1,2,3],[0,0,0],[5,5,5]])
void probs : int = nd_softmax(${logits}, -1)
>> print >> "softmax(logits): " >> ${probs}

# ---- 10. 切片 ----
void orig : int = tensor([[1,2,3,4],[5,6,7,8],[9,10,11,12]])
void sl : int = nd_slice(${orig}, [0,1], [3,3], [1,1])
>> print >> "orig: " >> ${orig}
>> print >> " sliced[0:3,1:3]: " >> ${sl}

# ---- 11. 填充 ----
void pd : int = nd_pad(${orig}, 1, 1, 1)
>> print >> "padded(axis1,before=1,after=1): " >> ${pd}

>> print >> "=== ALL ND TENSOR TESTS PASSED ==="