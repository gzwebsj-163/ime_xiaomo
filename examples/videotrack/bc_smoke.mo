# BC smoke test: 数组字面量 + while 循环 + 下标访问 (mo2kbc 可编译范围确认)
void data : int = [5, 9, 2, 7, 3, 8, 1, 6, 4, 0]
void idx : int = 0
void sum : int = 0
while ${idx} < 10:
    void sum : int = ${sum} + ${data}[${idx}]
    void idx : int = ${idx} + 1

>> print >> "SUM=" >> ${sum}
>> print >> "DATA[0]=" >> ${data}[0]
>> print >> "DATA[9]=" >> ${data}[9]
