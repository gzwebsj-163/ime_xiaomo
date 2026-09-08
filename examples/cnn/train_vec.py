import numpy as np, json, os
np.random.seed(42)

def gen_batch(n, quadrant=None):
    X,Y=[],[]
    for _ in range(n):
        img = np.random.rand(8,8)*0.3
        q = np.random.randint(4) if quadrant is None else quadrant
        cy = 1+(q//2)*4; cx=1+(q%2)*4
        img[cy:cy+2,cx:cx+2]=1.0
        X.append(img); Y.append(q)
    return np.array(X), np.array(Y)

class TinyCNN:
    def __init__(self, seed=42):
        rng = np.random.RandomState(seed)
        self.w1 = rng.randn(2,1,3,3)*0.5
        self.b1 = np.zeros(2)
        self.fw = rng.randn(18,4)*0.5
        self.fb = np.zeros(4)

    def im2col(self, x, kh=3,kw=3,sh=1,sw=1):
        # x: [N,8,8] -> col [N*OH*OW, kh*kw]
        N,H,W = x.shape; OH=H-kh+1; OW=W-kw+1
        col = np.empty((N,OH,OW,kh,kw))
        for i in range(kh):
            for j in range(kw):
                col[:,:,:,i,j] = x[:,i:i+OH,j:j+OW]
        return col.reshape(N*OH*OW, kh*kw)

    def forward(self, X, cache=False):
        # X:[N,8,8]
        N = X.shape[0]
        col = self.im2col(X)                      # [36N,9]
        conv = col @ self.w1.reshape(2,9).T + self.b1  # [36N,2]
        conv = conv.reshape(N,2,6,6)
        relu = np.maximum(conv,0)
        pool = relu.reshape(N,2,3,2,3,2).max(axis=(3,5))  # [N,2,3,3]
        flat = pool.reshape(N,18)
        logits = flat @ self.fw + self.fb         # [N,4]
        e = np.exp(logits-logits.max(axis=1,keepdims=True))
        p = e/e.sum(axis=1,keepdims=True)
        if cache: self._c=(col,conv,relu,pool,flat,logits,X)
        return p

    def backward(self, X, Y):
        col,conv,relu,pool,flat,logits,X = self._c
        N = X.shape[0]
        probs = np.exp(logits-logits.max(axis=1,keepdims=True))
        probs /= probs.sum(axis=1,keepdims=True)
        probs[np.arange(N),Y] -= 1
        dflat = probs @ self.fw.T                # [N,18]
        self.fw_g = flat.T @ probs / N
        self.fb_g = probs.sum(0)/N
        dpool = dflat.reshape(N,2,3,3)
        drelu = np.zeros_like(relu)
        for n in range(N):
            for o in range(2):
                for oh in range(3):
                    for ow in range(3):
                        blk = relu[n,o,oh*2:oh*2+2,ow*2:ow*2+2]
                        mi = np.unravel_index(np.argmax(blk),blk.shape)
                        drelu[n,o,oh*2+mi[0],ow*2+mi[1]] += dpool[n,o,oh,ow]
        dconv = drelu*(conv>0)                   # [N,2,6,6]
        # dw1: 对每个样本, conv 反传到输入窗口
        dconv_flat = dconv.reshape(N,2,36)       # 36=6*6
        dw1 = np.zeros((2,9))
        for n in range(N):
            dw1 += dconv_flat[n] @ col[n*36:(n+1)*36]  # [2,36]@[36,9]->[2,9]
        self.w1_g = dw1.reshape(2,1,3,3)/N
        self.b1_g = dconv.sum(axis=(0,2,3))/N

    def step(self, lr):
        self.w1-=lr*self.w1_g; self.b1-=lr*self.b1_g
        self.fw-=lr*self.fw_g; self.fb-=lr*self.fb_g

def run(Xtr,Ytr,Xte,Yte,lr,mom,bs,eps):
    m=TinyCNN(); v={k:0 for k in 'w1 b1 fw fb'.split()}
    n=len(Xtr)
    for ep in range(eps):
        perm=np.random.permutation(n); tl=0; tc=0
        for s in range(0,n,bs):
            idx=perm[s:s+bs]; X=Xtr[idx]; Y=Ytr[idx]
            p=m.forward(X,cache=True); tl+= -np.log(p[np.arange(len(idx)),Y]+1e-12).sum(); tc+=(p.argmax(1)==Y).sum()
            m.backward(X,Y)
            for k,g in [('w1',m.w1_g),('b1',m.b1_g),('fw',m.fw_g),('fb',m.fb_g)]:
                v[k]=mom*v[k]+lr*g
            m.w1-=v['w1'];m.b1-=v['b1'];m.fw-=v['fw'];m.fb-=v['fb']
        if (ep+1)%20==0 or ep==0:
            print(f" ep{ep+1:4d} loss={tl/n:.4f} acc={tc/n:.3f}")
    p=m.forward(Xte); acc=(p.argmax(1)==Yte).mean()
    print(f"  TEST acc={acc:.3f}")
    return m

Xtr,Ytr=gen_batch(400); Xte,Yte=gen_batch(200)
for lr,mom,bs in [(0.1,0.9,64),(0.05,0.9,64),(0.2,0.5,32)]:
    print(f"===== lr={lr} mom={mom} bs={bs} =====")
    run(Xtr,Ytr,Xte,Yte,lr,mom,bs,100)
