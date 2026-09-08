# ch340_cap.mo — 能力探测: 验证 CH340 仿真所需的语法特性
# 用法: ./xiaomo run examples/ch340_cap.mo

# 1. 函数 + 参数 + 返回值
fn power_status(in_ok):
    if ${in_ok} == 1:
        return 1
    return 0

void p : int = power_status(1)
>> print >> "P1 power=" >> ${p}

# 2. 字符串拼接 (多参数 print)
>> print >> "CH340C" >> " #" >> 1 >> " ready"

# 3. 数组遍历
void vbus : int = [5, 5, 5, 5]
void i : int = 0
while ${i} < 4:
    >> print >> "P2 ch" >> ${i} >> " vbus=" >> ${vbus}[${i}]
    void i : int = ${i} + 1

# 4. 取模 / 波特率计算
void baud : int = 115200
void divk : int = ${baud} / 1000
>> print >> "P3 baud_k=" >> ${divk}

# 5. 多条件嵌套 (判断端口在线)
void port : int = 2
if ${port} >= 0:
    if ${port} < 4:
        >> print >> "P4 port" >> ${port} >> " online"

# 6. 状态机: 计算 3.3V 是否正常 (5V 输入, LDO 效率 66%)
void vin : int = 5
void vout : int = ${vin} * 66 / 100
>> print >> "P5 vout=" >> ${vout} >> "V"

>> print >> "CAP_DONE"
