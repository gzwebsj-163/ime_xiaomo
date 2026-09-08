# ch340_dock.mo — CH340 4路编程扩展坞 · xiaomo VM 硬件仿真
# 用法: ./xiaomo run examples/ch340_dock.mo
#
# 在 xiaomo 虚拟机里完整模拟扩展坞工作流:
#   1. 电源轨: USB 5V -> 自恢复保险丝 -> AMS1117-3.3 稳压
#   2. GL3520 HUB: 枚举 4 个下游 CH340C
#   3. 4x CH340C 串口: 波特率配置 + UART 收发计数
#   4. ISP 烧录: STM32 (0x7F 握手) / 8051 (0x46/0xB9 同步)
#
# 所有状态用确定值推进, 可加断言验证, 用于教学/测试扩展坞设计。

# ================= 电源模型 =================
# 5V 输入, AMS1117 稳压到 3.3V, 过压(>6V)熔断保险丝
void vin : int = 5
void fuse_ok : int = 1
void vout : int = ${vin} * 66 / 100          # LDO 效率 66% -> 3.3V
>> print >> "PWR vin=" >> ${vin} >> "V fuse=" >> ${fuse_ok} >> " vout=" >> ${vout} >> "V"

# 过压测试: 12V 输入熔断保险丝
void vin_bad : int = 12
if ${vin_bad} > 6:
    void fuse_bad : int = 0                 # 严重过压熔断
else:
    void fuse_bad : int = 1
void vout_bad : int = 0
>> print >> "PWR_OV vin=" >> ${vin_bad} >> "V fuse=" >> ${fuse_bad} >> " vout=" >> ${vout_bad} >> "V"

# ================= GL3520 HUB 枚举 =================
# 4 个下游端口, 每个挂 CH340C @ 480Mbps, USB 地址 1~4
void hub : int = [1, 1, 1, 1]                # present 标志
void addr : int = [1, 2, 3, 4]
void enu_ok : int = 0
void hi : int = 0
while ${hi} < 4:
    if ${hub}[${hi}] == 1:
        void enu_ok : int = ${enu_ok} + 1
    >> print >> "HUB port" >> ${hi} >> " CH340C@480Mbps addr=" >> ${addr}[${hi}]
    void hi : int = ${hi} + 1
>> print >> "HUB_ENUM ports_ok=" >> ${enu_ok} >> "/4 host_detected=YES"

# ================= 4x CH340C 串口配置 =================
# 通道 0: STM32 ISP @115200, 通道 1: 8051 ISP @9600
# 通道 2: STM32 ISP @115200, 通道 3: DEBUG @115200
void baud : int = [115200, 9600, 115200, 115200]
void baud_k : int = [0, 0, 0, 0]
void ci : int = 0
while ${ci} < 4:
    baud_k[${ci}] = ${baud}[${ci}] / 1000    # 波特率 kbps
    >> print >> "CH" >> ${ci} >> " baud=" >> ${baud_k}[${ci}] >> "k"
    void ci : int = ${ci} + 1

# ================= STM32 ISP 烧录 (CH0) =================
# 流程: 握手 0x7F -> 回 ACK 0x79 -> 写内存 0x31+addr -> 校验 0x71
# 字节数: 握手1 + 写内存命令1 + 地址4 + 校验2 = 8 字节, 收 1 ACK
void stm_sent : int = 8
void stm_recv : int = 1
void stm_done : int = 1
>> print >> "STM32_ISP ch0 sent=" >> ${stm_sent} >> " recv=" >> ${stm_recv} >> " DONE"

# ================= 8051 ISP 烧录 (CH1) =================
# STC 冷启动: DTR 拉低再拉高, 同步头 0x46/0xB9 交替 4 轮
# 字节: 同步8(4轮x2) + 数据块(4KB/256=16块各1长度字节) = 24 字节, 收 8 同步
void code_size : int = 4096
void sync_tx : int = 8
void sync_rx : int = 8
void block_len : int = ${code_size} / 256
void data_bytes : int = ${block_len} * 1
void mcu_sent : int = ${sync_tx} + ${data_bytes}
void mcu_done : int = 1
>> print >> "8051_ISP ch1 code=" >> ${code_size} >> "B sent=" >> ${mcu_sent} >> " recv=" >> ${sync_rx} >> " DONE"

# ================= 汇总状态 =================
# 4 路通道最终状态统计
void done_count : int = ${stm_done} + ${mcu_done}
void total_sent : int = ${stm_sent} + ${mcu_sent}
void total_recv : int = ${stm_recv} + ${sync_rx}
>> print >> "DOCK total channels_done=" >> ${done_count} >> "/2 total_sent=" >> ${total_sent} >> " recv=" >> ${total_recv}

# 断言: 所有关键状态应符合预期
void pass : int = 1
if ${vout} != 3:                               # 5V*66% = 3.3, 整数截断为 3
    void pass : int = 0
if ${enu_ok} != 4:
    void pass : int = 0
if ${stm_done} != 1:
    void pass : int = 0
if ${mcu_done} != 1:
    void pass : int = 0

>> print >> "CHECK " >> ${pass}

>> print >> "DOCK_DONE"
