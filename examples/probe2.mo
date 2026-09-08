# probe2.mo — print 形式测试
void a : str = "user"
void b : int = 42

# P1
>> print >> "P1 literal only"
# P2: 单变量
>> print >> ${a}
# P3: 字面量 >> 变量
>> print >> "P3 role=" >> ${a}
# P4: 变量 >> 字面量 >> 变量
>> print >> ${a} >> "|" >> ${b}
# P5: 字面量 >> 变量 >> 字面量 >> 变量
>> print >> "P5 " >> ${a} >> " n=" >> ${b}
>> print >> "PROBE2_DONE"
