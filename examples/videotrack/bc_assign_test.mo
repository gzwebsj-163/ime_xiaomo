# BC 数组元素赋值测试: a[i] = v 循环写数组, 验证 STORE64 下沉
void arr : int = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
void i : int = 0
while ${i} < 10:
    arr[${i}] = ${i} * ${i}
    void i : int = ${i} + 1

void s : int = 0
void j : int = 0
while ${j} < 10:
    void s : int = ${s} + arr[${j}]
    void j : int = ${j} + 1

>> print >> "SQUARES_SUM=" >> ${s}
>> print >> "ARR[7]=" >> arr[7]
