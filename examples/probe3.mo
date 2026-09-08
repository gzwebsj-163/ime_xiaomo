# probe3.mo — 函数内数组操作验证 (openclaw-mini 数据结构关键)
void arr : int = [10, 20, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0]
void cnt : int = 3

# F1: 函数内数组元素赋值
fn set_at(i, v):
    arr[${i}] = ${v}
    return 1

# F2: 函数内读全局数组
fn get_at(i):
    return ${arr}[${i}]

# F3: 函数内重声明同名变量(模拟累加) 并返回
fn bump(c):
    void c : int = ${c} + 1
    void c : int = ${c} + 1
    return ${c}

# 主流程
void r : int = set_at(2, 99)
>> print >> "F1 set_at ret=" >> ${r}
>> print >> "F2 get_at(2)=" >> get_at(2)
void b : int = bump(5)
>> print >> "F3 bump(5)=" >> ${b}

# F4: 数组元素赋值配合游标 append
void i0 : int = 0
while ${i0} < 3:
    arr[${cnt}] = ${i0}
    void cnt : int = ${cnt} + 1
    void i0 : int = ${i0} + 1
>> print >> "F4 append后 cnt=" >> ${cnt} >> " arr[3]=" >> ${arr}[3] >> " arr[4]=" >> ${arr}[4] >> " arr[5]=" >> ${arr}[5]

# F5: 数组搬运 (prune 用左移)
void k : int = 1
while ${k} < 6:
    arr[${k} - 1] = arr[${k}]
    void k : int = ${k} + 1
>> print >> "F5 左移后 arr[0]=" >> ${arr}[0] >> " arr[1]=" >> ${arr}[1]
>> print >> "PROBE3_DONE"
