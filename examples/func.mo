# xiaomo 示例 3: 函数 + 递归 (factorial)
# 用法: ./xiaomo run examples/func.mo

# 阶乘函数 (递归)
fn fact(n):
    if ${n} <= 1:
        return 1
    return ${n} * fact(${n} - 1)

# 两数相加
fn sum(a, b):
    return ${a} + ${b}

>> print >> fact(5)
>> print >> fact(3)
>> print >> sum(2, 3)
