# BC 视频推理小规模模板测试: 6x6 两帧差分 -> 列/行投影 -> 运动质心
# 帧0 (全0), 帧1 (中央亮块), 期望: 质心在中央 (3.0, 3.0) 附近
void w : int = 6
void h : int = 6
void thr : int = 10
# 帧0: 6x6 全 0
void f0 : int = [0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0]
# 帧1: 中央 2x2 亮块 (值 100)
void f1 : int = [0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,100,100,0,0, 0,0,100,100,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0]
# 列/行投影
void col : int = [0,0,0,0,0,0]
void row : int = [0,0,0,0,0,0]
void y : int = 0
void x : int = 0
void i : int = 0
# 差分 + 阈值 + 投影
while ${y} < ${h}:
    void x : int = 0
    while ${x} < ${w}:
        void diff : int = f1[${y}*${w}+${x}] - f0[${y}*${w}+${x}]
        if ${diff} < 0:
            diff = 0 - ${diff}
        if ${diff} > ${thr}:
            col[${x}] = col[${x}] + ${diff}
            row[${y}] = row[${y}] + ${diff}
        void x : int = ${x} + 1
    void y : int = ${y} + 1

# 质心 cx = sum(x*col[x])/sum(col)
void nxx : int = 0
void dxx : int = 0
void k : int = 0
while ${k} < ${w}:
    void nxx : int = ${nxx} + ${k} * col[${k}]
    void dxx : int = ${dxx} + col[${k}]
    void k : int = ${k} + 1

void nyy : int = 0
void dyy : int = 0
void kk : int = 0
while ${kk} < ${h}:
    void nyy : int = ${nyy} + ${kk} * row[${kk}]
    void dyy : int = ${dyy} + row[${kk}]
    void kk : int = ${kk} + 1

>> print >> "CX=" >> ${nxx} >> "DX=" >> ${dxx}
>> print >> "CY=" >> ${nyy} >> "DY=" >> ${dyy}
