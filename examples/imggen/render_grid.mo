void W1 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "W1")
void b1 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "b1")
void W2 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "W2")
void b2 : int = load_weights("examples/imggen/output/imggen_xiaomo.json", "b2")

void grid : int = 128
void total : int = ${grid} * ${grid}
void R : int = mat_zeros(${total}, 1)
void G : int = mat_zeros(${total}, 1)
void B : int = mat_zeros(${total}, 1)
void idx : int = 0
void i : int = 0
while ${i} < ${grid}:
    void j : int = 0
    while ${j} < ${grid}:
        void uval : int = ${j} / 127.0
        void vval : int = 1.0 - ${i} / 127.0
        void inp : int = [0.0, 0.0, 1.0, 0.0, 0.0, ${uval}, ${vval}]
        void z1 : int = matmul(${inp}, ${W1})
        void a1 : int = bias_add(${z1}, ${b1})
        void h : int = tanh(${a1})
        void z2 : int = matmul(${h}, ${W2})
        void a2 : int = bias_add(${z2}, ${b2})
        void y : int = sigmoid(${a2})
        R[idx] = ${y}[0][0]
        G[idx] = ${y}[0][1]
        B[idx] = ${y}[0][2]
        void idx : int = ${idx} + 1
        void j : int = ${j} + 1
    void i : int = ${i} + 1

>> print >> "total=" >> ${total}
>> print >> "R[0]=" >> R[0] >> " G[0]=" >> G[0] >> " B[0]=" >> B[0]
>> print >> "R[16383]=" >> R[4095] >> " G[16383]=" >> G[4095] >> " B[16383]=" >> B[4095]
