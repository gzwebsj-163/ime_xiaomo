# probe4.mo — 函数内修改全局变量语义验证 (agent 循环关键)
void g : int = 10

# P1: 函数内裸名赋值修改全局
fn inc():
    g = ${g} + 1
    return ${g}

# P2: 函数内重声明同名(是否遮蔽全局)
fn dec():
    void g : int = ${g} - 1
    return ${g}

# P3: 函数内修改全局数组(带游标)
void arr : int = [0, 0, 0, 0, 0, 0, 0, 0]
void n : int = 0
fn addv(v):
    arr[${n}] = ${v}
    return 1

>> print >> "P1 inc()=" >> inc()
>> print >> "P1 后g=" >> ${g}
>> print >> "P2 dec()=" >> dec()
>> print >> "P2 后g=" >> ${g}
>> print >> "P3 push(7) n=" >> ${n}
void r1 : int = addv(7)
void n2 : int = ${n} + 1
>> print >> "P3 手动n+1=" >> ${n2} >> " arr[0]=" >> ${arr}[0]
>> print >> "PROBE4_DONE"
