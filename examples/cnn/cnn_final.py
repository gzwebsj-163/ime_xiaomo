import numpy as np, json, os
np.random.seed(42)
exec(open('train_vec.py').read().split('def run')[0])

Xtr,Ytr=gen_batch(400); Xte,Yte=gen_batch(200)
m=TinyCNN(seed=42); v={k:0 for k in 'w1 b1 fw fb'.split()}
n=len(Xtr); lr=0.1; mom=0.9; bs=64; eps=60
for ep in range(eps):
    perm=np.random.permutation(n)
    for s in range(0,n,bs):
        idx=perm[s:s+bs]; X=Xtr[idx]; Y=Ytr[idx]
        m.forward(X,cache=True); m.backward(X,Y)
        for k,g in [('w1',m.w1_g),('b1',m.b1_g),('fw',m.fw_g),('fb',m.fb_g)]:
            v[k]=mom*v[k]+lr*g
        m.w1-=v['w1'];m.b1-=v['b1'];m.fw-=v['fw'];m.fb-=v['fb']
p=m.forward(Xte); acc=(p.argmax(1)==Yte).mean()
print(f"train done. TEST acc={acc:.3f}")

w={"conv1_w":m.w1.tolist(),"conv1_b":m.b1.tolist(),"fc_w":m.fw.tolist(),"fc_b":m.fb.tolist()}
json.dump(w,open("cnn_weights.json","w"))
print("weights -> cnn_weights.json")

# 保存 4 个测试样本 + numpy 参考输出 (供 .mo 逐位对拍)
demo=[]
for q in range(4):
    x,y=gen_batch(1,quadrant=q)
    p=m.forward(x)
    demo.append({"input":x[0].tolist(),"label":int(y[0]),"probs":p[0].tolist(),
                 "conv":None})
json.dump(demo,open("cnn_samples.json","w"))
for d in demo:
    print(f"label={d['label']} argmax={np.argmax(d['probs'])} probs={[round(v,5) for v in d['probs']]}")
