# xiaomo 示例 1: 基础语法 (依据 lang_spec.md)
# 用法: ./xiaomo run examples/hello.mo

# 1. 变量声明
void message : str = "Hello, xiaomo!"
void x : int = 10
void y : int = 20

>> print >> message
>> print >> x
>> print >> y

# 2. 算术
void sum : int = ${x} + ${y}
>> print >> ${sum}

# 3. 条件分支
if ${x} > ${y}:
    >> print >> "x is greater than y"
else:
    >> print >> "x is not greater than y"
