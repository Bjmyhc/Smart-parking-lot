# -*- coding: utf-8 -*-
"""p565f: 改进合成渲染(粗体/正确长宽比/裁边/更真实的底色与反光), 重新训练 + 真实集验证."""
import os, sys, glob, random, json
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F
from PIL import Image, ImageDraw, ImageFont
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
FONT = r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\NotoSansCJK-Regular.ttc"
PROV = list("京津冀晋蒙辽吉黑沪苏浙皖闽赣鲁豫鄂湘粤桂琼渝川贵云藏陕甘青宁新")
W, H = 32, 64
random.seed(0); np.random.seed(0); torch.manual_seed(0)
_f = {}
def font(px):
    if px not in _f: _f[px] = ImageFont.truetype(FONT, px)
    return _f[px]
GLYPH = {}
def glyph_rgba(ch, px=192):
    """高分辨率渲染单个字 -> 灰度图(已裁到字的外接框)"""
    k = (ch, px)
    if k not in GLYPH:
        img = Image.new("L", (px*2, px*2), 0); d = ImageDraw.Draw(img)
        f = font(px)
        d.text((px//2, px//2), ch, fill=255, font=f, stroke_width=max(1, px//18), stroke_fill=255)
        a = np.asarray(img, np.uint8)
        ys, xs = np.nonzero(a > 8)
        GLYPH[k] = a[ys.min():ys.max()+1, xs.min():xs.max()+1]
    return GLYPH[k]
def render(ch, rng):
    SS = 4
    big = np.zeros((H*SS, W*SS, 3), np.float32)
    gy = np.linspace(-1, 1, H*SS)[:, None]
    # 底色: 蓝底, 屏摄下常常偏亮/发白
    lift = rng.uniform(0.0, 55.0)
    big[:,:,0] = 200.0 + gy*16 + lift
    big[:,:,1] = 148.0 + gy*14 + lift
    big[:,:,2] = 74.0 + gy*10 + lift
    g = glyph_rgba(ch)
    tw = rng.uniform(0.55, 0.78) * (W*SS)      # 字宽 = 屏宽的 55~78%
    th = rng.uniform(0.80, 0.94) * (H*SS)      # 字高
    gs = cv2.resize(g, (max(4,int(tw)), max(6,int(th))), interpolation=cv2.INTER_AREA)
    m = np.zeros((H*SS, W*SS), np.float32)
    ox = (W*SS - gs.shape[1])//2 + int(rng.uniform(-2, 2))
    oy = (H*SS - gs.shape[0])//2 + int(rng.uniform(-2, 2))
    x0, y0 = max(0, ox), max(0, oy)
    sx0, sy0 = max(0, -ox), max(0, -oy)
    ww = min(gs.shape[1]-sx0, W*SS-x0); hh = min(gs.shape[0]-sy0, H*SS-y0)
    if ww > 0 and hh > 0:
        m[y0:y0+hh, x0:x0+ww] = gs[sy0:sy0+hh, sx0:sx0+ww]/255.0
    # 白字不是纯白: 泛白/偏蓝都有
    fg = np.array([rng.uniform(225, 255), rng.uniform(230, 255), rng.uniform(235, 255)])
    img = big*(1-m[:,:,None]) + fg[None,None,:]*m[:,:,None]
    img = np.clip(img, 0, 255).astype(np.uint8)
    return cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)
def augment(im, rng):
    o = im.astype(np.float32)
    if rng.random() < 0.85:
        f = rng.uniform(0.30, 1.0)
        o = cv2.resize(cv2.resize(o, (max(4,int(W*f)), max(8,int(H*f))), interpolation=cv2.INTER_AREA), (W,H), interpolation=cv2.INTER_LINEAR)
    if rng.random() < 0.8:
        k = rng.choice([3,3,5]); o = cv2.GaussianBlur(o, (k,k), rng.uniform(0.5, 2.0))
    o = o * rng.uniform(0.72, 1.28)
    o = (o - 128.0) * rng.uniform(0.65, 1.45) + 128.0
    o = 255.0*np.power(np.clip(o,0,255)/255.0, rng.uniform(0.7, 1.4))
    for i in range(3): o[:,:,i] *= rng.uniform(0.85, 1.15)
    if rng.random() < 0.5:
        cx, cy = rng.uniform(0,W), rng.uniform(0,H*0.7); rr = rng.uniform(6, 22)
        yy, xx = np.mgrid[0:H,0:W]
        o = o + np.exp(-(((xx-cx)**2+(yy-cy)**2)/(2*rr*rr)))[:,:,None]*rng.uniform(40,150)
    if rng.random() < 0.7: o = o + np.random.normal(0, rng.uniform(2,11), o.shape)
    o = np.clip(o, 0, 255)
    if rng.random() < 0.55:
        M = cv2.getRotationMatrix2D((W/2,H/2), rng.uniform(-8,8), rng.uniform(0.9,1.1))
        M[0,2] += rng.uniform(-2.5,2.5); M[1,2] += rng.uniform(-3,3)
        o = cv2.warpAffine(o, M, (W,H), borderMode=cv2.BORDER_REPLICATE)
    if rng.random() < 0.55:
        ok, buf = cv2.imencode(".jpg", o.astype(np.uint8), [int(cv2.IMWRITE_JPEG_QUALITY), int(rng.uniform(30,90))])
        if ok: o = cv2.imdecode(buf, cv2.IMREAD_COLOR).astype(np.float32)
    return np.clip(o,0,255).astype(np.uint8)
class Net(nn.Module):
    def __init__(self, n=31):
        super().__init__()
        def blk(i,o): return nn.Sequential(nn.Conv2d(i,o,3,padding=1,bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))
        self.f = nn.Sequential(blk(3,16), nn.MaxPool2d(2), blk(16,32), nn.MaxPool2d(2), blk(32,64), nn.MaxPool2d(2), blk(64,64))
        self.head = nn.Linear(128, n)
    def forward(self, x):
        y = self.f(x)
        return self.head(torch.cat([F.adaptive_avg_pool2d(y,1).flatten(1), F.adaptive_max_pool2d(y,1).flatten(1)],1))
def to_t(im): return torch.from_numpy(((im[:,:,::-1].astype(np.float32)/255.0-0.5)/0.25).transpose(2,0,1))
real = []
for ch, lab in [("京","jing"),("粤","yue"),("豫","yu")]:
    i = PROV.index(ch)
    for p in sorted(glob.glob(os.path.join(OUT,"real_all",lab,"*.png"))):
        im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
        if im is not None: real.append((im, i, ch))
print("真实 %d 张" % len(real))
net = Net(); opt = torch.optim.Adam(net.parameters(), lr=2e-3, weight_decay=1e-5)
sched = torch.optim.lr_scheduler.StepLR(opt, 6, 0.4)
log = []
for ep in range(1, 17):
    net.train(); rng = random.Random(500+ep); X, Y = [], []
    for i, ch in enumerate(PROV):
        for _ in range(200):
            X.append(to_t(augment(render(ch, rng), rng))); Y.append(i)
    idx = torch.randperm(len(X)); tot = 0.0; nb = 0
    for b in range(0, len(X), 64):
        sel = idx[b:b+64]
        opt.zero_grad()
        out = net(torch.stack([X[j] for j in sel]))
        loss = F.cross_entropy(out, torch.tensor([Y[j] for j in sel]))
        loss.backward(); opt.step(); tot += loss.item(); nb += 1
    sched.step()
    net.eval()
    with torch.no_grad():
        pr = net(torch.stack([to_t(im) for im,_,_ in real])).argmax(1).numpy()
    per = {c:[0,0] for c in "京粤豫"}
    for (im,i,ch),p in zip(real,pr): per[ch][1]+=1; per[ch][0]+=int(p==i)
    acc = sum(v[0] for v in per.values())/max(1,len(real))
    line = "ep %2d loss %.3f | 真实 %5.1f%%  " % (ep, tot/max(1,nb), 100*acc) + "  ".join("%s %d/%d" % (c,per[c][0],per[c][1]) for c in "京粤豫")
    print(line); log.append(line)
open(os.path.join(OUT,"train_proto2.txt"),"w",encoding="utf-8").write("\n".join(log))
torch.save(net.state_dict(), os.path.join(OUT,"prov_net_synth2.pth"))
print("saved")
