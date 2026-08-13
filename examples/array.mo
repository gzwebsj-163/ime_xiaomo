# xiaomo 示例 4: 数组 (字面量 + 下标访问)
# 用法: ./xiaomo run examples/array.mo

# 数组字面量
void arr : int = [10, 20, 30]

>> print >> ${arr}
>> print >> ${arr}[0]
>> print >> ${arr}[1]
>> print >> ${arr}[2]

# 二维数组
void grid : int = [[1, 2], [3, 4]]
>> print >> ${grid}[1][1]
