# 由 make_cnn_kbc.py 生成: CNN 定点(Q16)推理, 一维数组 + WHILE 展开
# 语义 = 训练 TinyCNN (conv reshape 错位布局), argmax 分类

void x : int = [15249, 13543, 10171, 14504, 17284, 15591, 13600, 13798, 3167, 18691, 4044, 6861, 10184, 2481, 11670, 3592, 18154, 17472, 10704, 8837, 2017, 16187, 7459, 4843, 19568, 13139, 1452, 15754, 14754, 7888, 2432, 9780, 15007, 1495, 3213, 12169, 11001, 17795, 16019, 18065, 9161, 15489, 958, 5790, 12667, 65536, 65536, 9452, 4586, 3425, 17213, 2796, 8558, 65536, 65536, 2508, 13615, 6519, 8566, 12300, 7451, 15726, 19051, 18345]
void w1 : int = [81469, 7686, 31158, 124288, 24274, 21132, 140330, 6860, -37041, -332, 11357, 7368, -1012, -72882, -60817, -33198, -57266, -15196]
void b1 : int = [-71619, -101985]
void fw : int = [-25243, -50977, 42295, -1480, 126954, -104731, -61098, -19802, -15832, 128605, -101044, -66373, -13826, 59873, -6771, -33398, 88290, -70472, -15585, -72655, -15335, 57157, -26403, -22677, 2963, -1393, -61607, -25656, -1828, 32237, 841, -58216, 19606, -865, -39108, 16230, 38069, 29305, -26319, -14387, 2797, -5692, 83847, -59915, -45551, -82681, 6581, 117267, 2219, 35178, 6820, -22982, -2447, -5360, 164546, -44401, -106458, -51580, -407, 92589, 7125, -64331, -20032, 19620, 44770, -41971, 46621, -60909, 19565, -35339, 3023, 52978]
void fb : int = [5777, 12408, -32774, 14589]
void c : int = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
void p : int = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
void lg : int = [0, 0, 0, 0]

# ---- CONV1 (训练语义: loc=oc*36+i*6+j, win=loc/2, ch=loc%2) ----
void oc : int = 0
while ${oc} < 2:
    void oh : int = 0
    while ${oh} < 6:
        void ow : int = 0
        while ${ow} < 6:
            void loc : int = ${oc} * 36 + ${oh} * 6 + ${ow}
            void win : int = ${loc} / 2
            void ch : int = ${loc} - ${win} * 2
            void wh : int = ${win} / 6
            void ww : int = ${win} - ${wh} * 6
            void acc : int = 0
            void kh : int = 0
            while ${kh} < 3:
                void kw : int = 0
                while ${kw} < 3:
                    void acc : int = ${acc} + x[${wh} * 8 + ${ww} + ${kh} * 8 + ${kw}] * w1[${ch} * 9 + ${kh} * 3 + ${kw}]
                    void kw : int = ${kw} + 1
                void kh : int = ${kh} + 1
            void acc : int = ${acc} + b1[${ch}]
            if ${acc} < 0:
                void acc : int = 0
            c[${loc}] = ${acc}
            void ow : int = ${ow} + 1
        void oh : int = ${oh} + 1
    void oc : int = ${oc} + 1

# ---- MAXPOOL 2x2 s2 (c 为标准布局) ----
void oc : int = 0
while ${oc} < 2:
    void oh : int = 0
    while ${oh} < 3:
        void ow : int = 0
        while ${ow} < 3:
            void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2]
            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 1]:
                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 1]
            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 6]:
                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 6]
            if ${ma} < c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 7]:
                void ma : int = c[${oc} * 36 + ${oh} * 12 + ${ow} * 2 + 7]
            p[${oc} * 9 + ${oh} * 3 + ${ow}] = ${ma}
            void ow : int = ${ow} + 1
        void oh : int = ${oh} + 1
    void oc : int = ${oc} + 1

# ---- FC (18x4) ----
void fout : int = 0
while ${fout} < 4:
    void facc : int = fb[${fout}]
    void fk : int = 0
    while ${fk} < 18:
        void facc : int = ${facc} + p[${fk}] * fw[${fk} * 4 + ${fout}]
        void fk : int = ${fk} + 1
    lg[${fout}] = ${facc}
    void fout : int = ${fout} + 1

# ---- ARGMAX ----
void ai : int = 1
void av : int = lg[0]
void ab : int = 0
while ${ai} < 4:
    if ${av} < lg[${ai}]:
        void av : int = lg[${ai}]
        void ab : int = ${ai}
    void ai : int = ${ai} + 1

>> print >> "=== KBC CNN 定点推理 (Q16) ==="
>> print >> "logits=" >> lg[0] >> "," >> lg[1] >> "," >> lg[2] >> "," >> lg[3]
>> print >> "argmax=" >> ${ab}
