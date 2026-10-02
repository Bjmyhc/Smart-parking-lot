# -*- coding: utf-8 -*-
"""p573: 省字模型 —— 抗"抠图窗口横向偏位"重训版 (对比 p570 基线).

动机 (p572 实测): 同一批真实 patch, 只要横向挪 20%/40% 的窗口宽度,
  置信中位就从 94% 掉到 77~82%/39%, 粤 甚至变成 9% top-1; 而提亮/模糊/低分辨率都打不垮它.
  板上"同一块牌相邻帧 95% -> 22% -> 95%"正是这种偏位抖动的签名.
做法: 保持网络/量化友好的结构不变, 只在增广里把横向平移从 ±2.5px 放大到 ±30% 窗宽 (含越界复制),
      跑同样的 4 折交叉验证, 并在每折留出集上量"偏位鲁棒性曲线".
"""
import os, glob, random
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
PRE = os.path.join(OUT, "prov_net_synth2.pth")
src = open(os.path.join(ROOT, "tools", "p565f_train2.py"), encoding="utf-8").read()
ns = {}
exec(src[:src.index("real = []")], ns)
render, augment, to_t, PROV, W, H = ns["render"], ns["augment"], ns["to_t"], ns["PROV"], ns["W"], ns["H"]

class NetA(nn.Module):
    def __init__(self, n=31):
        super().__init__()
        def blk(i, o):
            return nn.Sequential(nn.Conv2d(i, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))
        self.f = nn.Sequential(blk(3, 16), nn.MaxPool2d(2), blk(16, 32), nn.MaxPool2d(2),
                               blk(32, 64), nn.MaxPool2d(2), blk(64, 64))
        self.head = nn.Linear(64, n)
    def forward(self, x):
        return self.head(F.adaptive_avg_pool2d(self.f(x), 1).flatten(1))

def rollf(u8, f):
    k = int(round(f * W))
    if k == 0: return u8
    if abs(k) >= W: return u8
    if k > 0:  return np.hstack([np.repeat(u8[:, :1], k, 1), u8[:, :-k]])
    k = -k
    return np.hstack([u8[:, k:], np.repeat(u8[:, -1:], k, 1)])

def augment2(im, rng):
    o = augment(im, rng)
    if rng.random() < 0.85:                      # 大范围横向偏位: 窗口对不齐的模拟
        o = rollf(o.astype(np.uint8), rng.uniform(-0.30, 0.30)).astype(np.float32)
    return o

def warm_start():
    sd = torch.load(PRE, map_location="cpu")
    net = NetA()
    net.load_state_dict({k: v for k, v in sd.items() if not k.startswith("head.")}, strict=False)
    with torch.no_grad():
        net.head.weight.copy_(sd["head.weight"][:, :64]); net.head.bias.copy_(sd["head.bias"])
    return net

def finetune(tr, augfn, epochs=10, seed=99, lr=4e-4, aug_p=0.6):
    net = warm_start()
    for m in net.modules():
        if isinstance(m, nn.BatchNorm2d): m.momentum = 0.05
    opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=1e-5)
    rng = random.Random(seed)
    REP = max(1, int(round(1200 / max(1, len(tr)))))
    for ep in range(epochs):
        net.train(); X, Y = [], []
        for i, ch in enumerate(PROV):
            for _ in range(120):
                X.append(to_t(augfn(render(ch, rng), rng))); Y.append(i)
        for _ in range(REP):
            for im, i in tr:
                X.append(to_t(augfn(im, rng) if rng.random() < aug_p else im)); Y.append(i)
        idx = torch.randperm(len(X))
        for b in range(0, len(X), 64):
            sel = idx[b:b + 64]
            opt.zero_grad()
            loss = F.cross_entropy(net(torch.stack([X[j] for j in sel])), torch.tensor([Y[j] for j in sel]))
            loss.backward(); opt.step()
    net.eval(); return net

def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

real = {}
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    real[ch] = [(load(p), PROV.index(ch)) for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png")))]
    real[ch] = [(im, i) for im, i in real[ch] if im is not None]

SHIFTS = [0.0, 0.10, -0.10, 0.20, -0.20, 0.30, -0.30, 0.40, -0.40]

def curve(net, va):
    gt = np.array([i for _, i in va])
    out = []
    for f in SHIFTS:
        X = [rollf(im, f) if f else im for im, _ in va]
        with torch.no_grad():
            pr = F.softmax(net(torch.stack([to_t(im) for im in X])), 1).numpy()
        pred, conf = pr.argmax(1), pr.max(1)
        out.append((f, 100.0 * (pred == gt).mean(), float(np.median(conf))))
    return out

old = NetA(); old.load_state_dict(torch.load(os.path.join(OUT, "prov_final_avg.pth"), map_location="cpu")); old.eval()

NF = 4
folds = {}
for ch, items in real.items():
    it = list(items); random.Random(7).shuffle(it)
    folds[ch] = [it[i::NF] for i in range(NF)]

lines = []
def say(s):
    print(s); lines.append(s)

say("每类: " + "  ".join("%s %d" % (c, len(v)) for c, v in real.items()))

tot_ok = tot_n = ok7 = n7 = 0
old_new_curve = []
for f in range(NF):
    tr = [x for ch in real for i in range(NF) if i != f for x in folds[ch][i]]
    va = [x for ch in real for x in folds[ch][f]]
    net = finetune(tr, augment2)
    gt = np.array([i for _, i in va])
    with torch.no_grad():
        pr = F.softmax(net(torch.stack([to_t(im) for im, _ in va])), 1).numpy()
    pred, conf = pr.argmax(1), pr.max(1)
    m = conf >= 0.70
    ok, n = int((pred == gt).sum()), len(gt)
    o7, n7f = int((pred[m] == gt[m]).sum()), int(m.sum())
    tot_ok += ok; tot_n += n; ok7 += o7; n7 += n7f
    say("折%d: 干净 top-1 %5.1f%% (%d/%d) | 置信>=0.70 %5.1f%% (覆盖 %.0f%%)" % (
        f + 1, 100.0 * ok / n, ok, n, 100.0 * o7 / max(1, n7f), 100.0 * n7f / n))
    old_new_curve.append((curve(net, va), curve(old, va)))
say("4 折合计: 干净 top-1 %5.1f%% (%d/%d) | 置信>=0.70 %5.1f%% (%d/%d, 覆盖 %.0f%%)" % (
    100.0 * tot_ok / tot_n, tot_ok, tot_n, 100.0 * ok7 / max(1, n7), ok7, n7, 100.0 * n7 / tot_n))

say("")
say("偏位鲁棒性曲线 (4 折留出集平均; 新=加强平移增广, 旧=p570 上板版)")
say("  偏移量   新top1%  新置信中位 |  旧top1%  旧置信中位")
for i, f in enumerate(SHIFTS):
    nA = np.mean([c[0][i][1] for c in old_new_curve]); nC = np.mean([c[0][i][2] for c in old_new_curve])
    oA = np.mean([c[1][i][1] for c in old_new_curve]); oC = np.mean([c[1][i][2] for c in old_new_curve])
    say("  %+5.0f%%   %6.1f   %6.1f%%  |  %6.1f   %6.1f%%" % (100 * f, nA, 100 * nC, oA, 100 * oC))

# ---- 全量出权重 (上板用) ----
net = finetune([x for ch in real for x in real[ch]], augment2)
torch.save(net.state_dict(), os.path.join(OUT, "prov_shift_avg.pth"))
with torch.no_grad():
    pr = F.softmax(net(torch.stack([to_t(im) for im, _ in [x for ch in real for x in real[ch]]])), 1).numpy()
gt = np.array([i for _, i in [x for ch in real for x in real[ch]]])
say("")
say("全量微调权重 -> prov_shift_avg.pth | 训练集自身 %5.1f%%" % (100.0 * (pr.argmax(1) == gt).mean()))
open(os.path.join(OUT, "p573_shift.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
