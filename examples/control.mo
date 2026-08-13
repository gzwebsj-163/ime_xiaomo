# xiaomo 示例 2: 控制流 (while 循环)
# 用法: ./xiaomo run examples/control.mo

# 循环求和 1..5
void counter : int = 0
void total : int = 0

while ${counter} < 5:
    void total : int = ${total} + ${counter}
    void counter : int = ${counter} + 1

>> print >> ${total}

# 模板引用直接输出
>> print >> [${counter}, ${total}]
