import numpy as np, json, os
np.random.seed(42)
exec(open('cnn_train_export.py').read().split('def main')[0])

def train(model, X, Y, epochs, lr, bs, mom):
    v = {'w1':0,'b1':0,'fw':0,'fb':0}
    n = len(X)
    for ep in range(epochs):
        perm = np.random.permutation(n)
        tot_loss=0; corr=0
        for s in range(0, n, bs):
            gw1=np.zeros_like(model.w1); gb1=np.zeros_like(model.b1)
            gfw=np.zeros_like(model.fw); gfb=np.zeros_like(model.fb)
            for i in perm[s:s+bs]:
                x,y = X[i],Y[i]
                p = model.forward(x)
                tot_loss += -np.log(p[y]+1e-12); corr += (np.argmax(p)==y)
                model.backward(x,y)
                gw1+=model.w1_g; gb1+=model.b1_g; gfw+=model.fw_g; gfb+=model.fb_g
            m = bs
            for name,g in [('w1',gw1/m),('b1',gb1/m),('fw',gfw/m),('fb',gfb/m)]:
                v[name] = mom*v[name] + lr*g
            model.w1 -= v['w1']; model.b1 -= v['b1']
            model.fw -= v['fw']; model.fb -= v['fb']
        if (ep+1)%20==0 or ep==0:
            print(f"ep {ep+1:4d} loss={tot_loss/n:.4f} acc={corr/n:.3f}")
    return tot_loss/n, corr/n

# 实验: 不同超参
Xtr,Ytr = gen_batch(400)
Xte,Yte = gen_batch(200)

for lr,mom,bs,eps in [(0.1,0.9,64,150),(0.05,0.9,64,300),(0.2,0.5,32,200)]:
    print(f"\n===== lr={lr} mom={mom} bs={bs} eps={eps} =====")
    m = TinyCNN()
    train(m,Xtr,Ytr,eps,lr,bs,mom)
    c=sum(np.argmax(m.forward(Xte[i]))==Yte[i] for i in range(len(Xte)))
    print(f"TEST acc={c/len(Xte):.3f}")
