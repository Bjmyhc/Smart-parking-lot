# -*- coding: utf-8 -*-
"""p571: 省字模型鲁棒性归因实验.

目的: 板上"复核置信塌到 41~64%"到底是 (a) 门槛太高, 还是 (b) patch 画质被反光/过曝/小占屏毁掉.
做法: 拿 245 张真实省字 patch (板上同一来源), 人为劣化 (提亮/泛白/模糊/低分辨率),
      量 top-1 与 softmax 置信, 看哪一类劣化能把 95% 打到 45%.
"""
import os, glob
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
PROV = list("京津冀晋蒙辽吉黑沪苏浙皖闽赣鲁豫鄂湘粤桂琼渝川贵云藏陕甘青宁新")
W, H = 32, 64

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

def to_t(im):
    return torch.from_numpy(((im[:, :, ::-1].astype(np.float32) / 255.0 - 0.5) / 0.25).transpose(2, 0, 1))

net = NetA()
net.load_state_dict(torch.load(os.path.join(OUT, "prov_final_avg.pth"), map_location="cpu"))
net.eval()

def load(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

items = []
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            items.append((im, PROV.index(ch), ch))
print("真实 patch %d 张: %s" % (len(items), {c: sum(1 for _, _, k in items if k == c) for c in "京粤豫"}))

# ---- 劣化算子 ----
def bright(im, a):            # 泛白/过曝: 向白靠拢
    return np.clip(im.astype(np.float32) + (255.0 - im.astype(np.float32)) * a, 0, 255).astype(np.uint8)

def blur(im, s):
    return im if s <= 0 else cv2.GaussianBlur(im, (0, 0), s)

def small(im, f):             # 占屏很小 -> 原生像素不够, 先降采样再插回来
    h, w = im.shape[:2]
    nw, nh = max(2, int(round(w * f))), max(2, int(round(h * f)))
    return cv2.resize(cv2.resize(im, (nw, nh), interpolation=cv2.INTER_AREA), (w, h), interpolation=cv2.INTER_LINEAR)

def oscore(im):
    return float(cv2.Laplacian(cv2.cvtColor(im, cv2.COLOR_BGR2GRAY), cv2.CV_64F).var())

CASES = [
    ("原样 (基准)",              lambda im: im),
    ("提亮 a=0.15",              lambda im: bright(im, 0.15)),
    ("提亮 a=0.30",              lambda im: bright(im, 0.30)),
    ("提亮 a=0.45",              lambda im: bright(im, 0.45)),
    ("模糊 s=0.7",               lambda im: blur(im, 0.7)),
    ("模糊 s=1.2",               lambda im: blur(im, 1.2)),
    ("低分辨率 f=0.5",           lambda im: small(im, 0.5)),
    ("低分辨率 f=0.33",          lambda im: small(im, 0.33)),
    ("低分辨率 f=0.25",          lambda im: small(im, 0.25)),
    ("提亮0.30+小占屏0.33",      lambda im: small(bright(im, 0.30), 0.33)),
    ("提亮0.30+模糊0.7",         lambda im: blur(bright(im, 0.30), 0.7)),
    ("提亮0.45+模糊0.7+小占屏0.33", lambda im: small(blur(bright(im, 0.45), 0.7), 0.33)),
]

def ev(fn):
    X = [fn(im) for im, _, _ in items]
    gt = np.array([i for _, i, _ in items])
    sharp = float(np.mean([oscore(im) for im in X]))
    with torch.no_grad():
        pr = F.softmax(net(torch.stack([to_t(im) for im in X])), 1).numpy()
    pred, conf = pr.argmax(1), pr.max(1)
    acc = 100.0 * (pred == gt).mean()
    out = {"acc": acc, "c50": float(np.median(conf)), "sharp": sharp}
    for t in (0.70, 0.55, 0.45, 0.35):
        m = conf >= t
        out["cov%d" % (t * 100)] = 100.0 * m.mean()
        out["acc%d" % (t * 100)] = (100.0 * (pred[m] == gt[m]).mean()) if m.sum() else float("nan")
    return out

print()
print("%-28s %6s %6s %7s | %s" % ("条件", "锐度", "top1%", "置信中位", "  门槛 0.70 覆盖/准确 | 0.55 覆盖/准确 | 0.45 覆盖/准确 | 0.35 覆盖/准确"))
rows = []
for name, fn in CASES:
    r = ev(fn)
    rows.append((name, r))
    def cell(t):
        return "%5.0f%%/%5.1f%%" % (r["cov%d" % (t * 100)], r["acc%d" % (t * 100)])
    print("%-28s %6.1f %6.1f %5.1f%% | %s | %s | %s | %s" % (
        name, r["sharp"], r["acc"], 100 * r["c50"], cell(0.70), cell(0.55), cell(0.45), cell(0.35)))

# ---- 分省看: 劣化后谁先崩 ----
print()
print("分省 top-1 (劣化后):")
for name, fn in CASES:
    per = {}
    for ch in "京粤豫":
        sub = [(im, i) for im, i, c in items if c == ch]
        with torch.no_grad():
            pr = F.softmax(net(torch.stack([to_t(fn(im)) for im, _ in sub])), 1).numpy()
        pred = pr.argmax(1)
        gt = np.array([i for _, i in sub])
        per[ch] = "%.0f%%(n=%d,中位%.0f%%)" % (100.0 * (pred == gt).mean(), len(sub), 100 * np.median(pr.max(1)))
    print("  %-28s %s" % (name, "  ".join("%s %s" % (c, per[c]) for c in "京粤豫")))
