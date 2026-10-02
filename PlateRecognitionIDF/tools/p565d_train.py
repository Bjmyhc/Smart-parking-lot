# -*- coding: utf-8 -*-
"""p565d: 省字分类器原型 —— 只用合成数据训练, 在 245 张真实省字上验证.
输入 32x64 (WxH) BGR, 输出 31 个省字。
"""
import os, sys, glob, json, random
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F
try:
    from PIL import Image, ImageDraw, ImageFont
except Exception as e:
    print("PIL 不可用:", e); sys.exit(1)

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565"); os.makedirs(OUT, exist_ok=True)
REAL = os.path.join(OUT, "real_all")
FONT = r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\NotoSansCJK-Regular.ttc"
PROV = list("京津冀晋蒙辽吉黑沪苏浙皖闽赣鲁豫鄂湘粤桂琼渝川贵云藏陕甘青宁新")
assert len(PROV) == 31, len(PROV)
W, H, SS = 32, 64, 4
random.seed(0); np.random.seed(0); torch.manual_seed(0)
_fonts = {}
def font(px):
    if px not in _fonts:
        _fonts[px] = ImageFont.truetype(FONT, px)
    return _fonts[px]

def render(ch):
    # 车牌底: BGR 蓝底 (真实屏摄里底色约 B205 G150 R75)
    big = np.zeros((H*SS, W*SS, 3), np.float32)
    base_b, base_g, base_r = 205.0, 150.0, 72.0
    gy = np.linspace(-1, 1, H*SS)[:, None]
    big[:, :, 0] = base_b + gy*18
    big[:, :, 1] = base_g + gy*16
    big[:, :, 2] = base_r + gy*12
    # 字: 用 PIL 画白字 -> 掩码
    m = Image.new("L", (W*SS, H*SS), 0)
    d = ImageDraw.Draw(m)
    f = font(int(H*SS*0.80))
    bb = d.textbbox((0, 0), ch, font=f)
    d.text(((W*SS-(bb[2]-bb[0]))/2 - bb[0], (H*SS-(bb[3]-bb[1]))/2 - bb[1]), ch, fill=255, font=f)
    mk = np.asarray(m, np.float32)[:, :, None] / 255.0
    img = big*(1-mk) + 250.0*mk
    img = np.clip(img, 0, 255).astype(np.uint8)
    return cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)

def augment(im, rng):
    o = im.astype(np.float32)
    # 分辨率损失(屏摄常见的"糊")
    if rng.random() < 0.8:
        f = rng.uniform(0.35, 1.0)
        o = cv2.resize(cv2.resize(o, (max(4,int(W*f)), max(8,int(H*f))), interpolation=cv2.INTER_AREA),
                       (W, H), interpolation=cv2.INTER_LINEAR)
    # 模糊
    if rng.random() < 0.7:
        k = rng.choice([3, 3, 5]); o = cv2.GaussianBlur(o, (k, k), rng.uniform(0.4, 1.6))
    # 亮度/对比度/伽马
    g = rng.uniform(0.75, 1.25); o = o * g
    c = rng.uniform(0.7, 1.4); o = (o - 128.0) * c + 128.0
    gm = rng.uniform(0.75, 1.35); o = 255.0 * np.power(np.clip(o, 0, 255)/255.0, gm)
    # 白平衡偏移
    for i in range(3):
        o[:, :, i] *= rng.uniform(0.88, 1.12)
    # 反光: 一块亮斑
    if rng.random() < 0.45:
        cx, cy = rng.uniform(0, W), rng.uniform(0, H*0.6)
        yy, xx = np.mgrid[0:H, 0:W]
        rr = rng.uniform(6, 20)
        blob = np.exp(-(((xx-cx)**2 + (yy-cy)**2) / (2*rr*rr)))
        o = o + blob[:, :, None] * rng.uniform(40, 130)
    # 噪声
    if rng.random() < 0.6:
        o = o + np.random.normal(0, rng.uniform(2, 9), o.shape)
    o = np.clip(o, 0, 255)
    # 旋转/平移
    if rng.random() < 0.5:
        M = cv2.getRotationMatrix2D((W/2, H/2), rng.uniform(-7, 7), rng.uniform(0.92, 1.08))
        M[0, 2] += rng.uniform(-2, 2); M[1, 2] += rng.uniform(-3, 3)
        o = cv2.warpAffine(o, M, (W, H), borderMode=cv2.BORDER_REPLICATE)
    # JPEG
    if rng.random() < 0.5:
        q = int(rng.uniform(35, 92))
        ok, buf = cv2.imencode(".jpg", o.astype(np.uint8), [int(cv2.IMWRITE_JPEG_QUALITY), q])
        if ok: o = cv2.imdecode(buf, cv2.IMREAD_COLOR).astype(np.float32)
    return np.clip(o, 0, 255).astype(np.uint8)

class Net(nn.Module):
    def __init__(self, n=31):
        super().__init__()
        def blk(i, o): return nn.Sequential(nn.Conv2d(i, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))
        self.f = nn.Sequential(blk(3,16), nn.MaxPool2d(2), blk(16,32), nn.MaxPool2d(2), blk(32,64), nn.MaxPool2d(2), blk(64,64))
        self.head = nn.Linear(128, n)
    def forward(self, x):
        y = self.f(x)
        a = F.adaptive_avg_pool2d(y, 1).flatten(1)
        m = F.adaptive_max_pool2d(y, 1).flatten(1)
        return self.head(torch.cat([a, m], 1))

def to_tensor(im):
    x = (im[:, :, ::-1].astype(np.float32) / 255.0 - 0.5) / 0.25      # BGR->RGB, 归一
    return torch.from_numpy(x.transpose(2, 0, 1))

def load_real():
    data = []
    for i, ch in enumerate(PROV):
        for p in sorted(glob.glob(os.path.join(REAL, {"京":"jing","粤":"yue","豫":"yu"}.get(ch, "___"), "*.png"))):
            im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
            if im is not None: data.append((im, i, ch))
    return data

real = load_real()
print("真实验证样本: %d (%s)" % (len(real), ", ".join("%s%d" % (c, sum(1 for _, _, x in real if x == c)) for c in "京粤豫")))
net = Net(); opt = torch.optim.Adam(net.parameters(), lr=2e-3)
sched = torch.optim.lr_scheduler.StepLR(opt, 5, 0.4)
EPOCHS, PER = 14, 200
log = []
for ep in range(1, EPOCHS+1):
    net.train(); tot = 0.0; nb = 0
    rng = random.Random(1000+ep)
    X, Y = [], []
    for i, ch in enumerate(PROV):
        for _ in range(PER):
            X.append(to_tensor(augment(render(ch), rng))); Y.append(i)
    idx = torch.randperm(len(X))
    for b in range(0, len(X), 64):
        sel = idx[b:b+64]
        xb = torch.stack([X[j] for j in sel]); yb = torch.tensor([Y[j] for j in sel])
        opt.zero_grad(); out = net(xb); loss = F.cross_entropy(out, yb)
        loss.backward(); opt.step(); tot += loss.item(); nb += 1
    sched.step()
    net.eval()
    with torch.no_grad():
        xs = torch.stack([to_tensor(im) for im, _, _ in real])
        pr = net(xs).argmax(1).numpy()
    per = {c: [0,0] for c in "京粤豫"}
    for (im, i, ch), p in zip(real, pr):
        per[ch][1] += 1; per[ch][0] += int(p == i)
    acc = sum(v[0] for v in per.values()) / max(1, len(real))
    line = "ep %2d loss %.3f | 真实总体 %5.1f%%  " % (ep, tot/max(1,nb), 100*acc) + \
           "  ".join("%s %d/%d" % (c, per[c][0], per[c][1]) for c in "京粤豫")
    print(line); log.append(line)
open(os.path.join(OUT, "train_proto.txt"), "w", encoding="utf-8").write("\n".join(log))
torch.save(net.state_dict(), os.path.join(OUT, "prov_net_synth.pth"))
print("saved", os.path.join(OUT, "prov_net_synth.pth"))
