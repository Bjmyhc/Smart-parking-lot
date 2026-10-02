# -*- coding: utf-8 -*-
"""p572: 省字模型"域差"归因第二轮 —— 色彩/白平衡/极端画质/几何偏移.

p571 已证: 提亮/模糊/低分辨率 都不足以把置信从 95% 打到 45%.
这一轮专查 (a) 色偏(白平衡漂移) (b) 极端对比度/gamma (c) 抠图偏位.
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

def f32(im): return np.clip(im.astype(np.float32), 0, 255)

def gain(im, kb=1.0, kg=1.0, kr=1.0):
    o = f32(im); o[:, :, 0] *= kb; o[:, :, 1] *= kg; o[:, :, 2] *= kr
    return np.clip(o, 0, 255).astype(np.uint8)

def gamma(im, g):
    return (255.0 * np.power(f32(im) / 255.0, g)).astype(np.uint8)

def contrast(im, c, mid=128.0):
    return np.clip((f32(im) - mid) * c + mid, 0, 255).astype(np.uint8)

def sat(im, s):
    g = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY).astype(np.float32)[:, :, None]
    return np.clip(g + (f32(im) - g) * s, 0, 255).astype(np.uint8)

def roll(im, f):                      # 抠图偏位: 横向挪 f*W 像素, 边缘复制
    k = int(round(f * W))
    if k == 0: return im
    if k > 0:  return np.hstack([np.repeat(im[:, :1], k, 1), im[:, :-k]]) if k < W else im
    k = -k
    return np.hstack([im[:, k:], np.repeat(im[:, -1:], k, 1)]) if k < W else im

def jpeg(im, q):
    ok, b = cv2.imencode(".jpg", im, [int(cv2.IMWRITE_JPEG_QUALITY), q])
    return cv2.imdecode(b, cv2.IMREAD_COLOR) if ok else im

def squeeze(im, s):                   # 窗口偏窄/偏宽: 内容被横向拉伸/压缩
    nw = max(2, int(round(W * s)))
    r = cv2.resize(im, (nw, H), interpolation=cv2.INTER_LINEAR)
    return cv2.resize(r, (W, H), interpolation=cv2.INTER_LINEAR)
def glare(im, a, cx=0.5, cy=0.35, r=0.7):
    h, w = im.shape[:2]; yy, xx = np.mgrid[0:h, 0:w]
    m = np.exp(-(((xx - cx * w) ** 2 + (yy - cy * h) ** 2) / (2 * (r * w) ** 2)))[:, :, None]
    return np.clip(f32(im) + (255.0 - f32(im)) * a * m, 0, 255).astype(np.uint8)

CASES = [
    ("基准",                      lambda im: im),
    ("B(蓝) x0.6",                lambda im: gain(im, kb=0.6)),
    ("B(蓝) x1.6",                lambda im: gain(im, kb=1.6)),
    ("R(红) x1.6",                lambda im: gain(im, kr=1.6)),
    ("R x0.6",                    lambda im: gain(im, kr=0.6)),
    ("G x1.6",                    lambda im: gain(im, kg=1.6)),
    ("蓝x0.7 红x1.5 (偏黄)",       lambda im: gain(im, kb=0.7, kr=1.5)),
    ("蓝x1.5 红x0.7 (偏青)",       lambda im: gain(im, kb=1.5, kr=0.7)),
    ("饱和度 x0.3",               lambda im: sat(im, 0.3)),
    ("饱和度 x0 (灰)",             lambda im: sat(im, 0.0)),
    ("gamma 0.5 (压暗)",          lambda im: gamma(im, 0.5)),
    ("gamma 1.8 (提亮)",          lambda im: gamma(im, 1.8)),
    ("对比度 x0.4",               lambda im: contrast(im, 0.4)),
    ("对比度 x2.0",               lambda im: contrast(im, 2.0)),
    ("JPEG q=15",                 lambda im: jpeg(im, 15)),
    ("强反光斑 a=0.7",             lambda im: glare(im, 0.7)),
    ("抠图偏位 +20%",             lambda im: roll(im, 0.20)),
    ("抠图偏位 -20%",             lambda im: roll(im, -0.20)),
    ("抠图偏位 +40%",             lambda im: roll(im, 0.40)),
    ("抠图偏位 -40%",             lambda im: roll(im, -0.40)),
    ("偏位+20% & 蓝x0.7红x1.5",    lambda im: roll(gain(im, kb=0.7, kr=1.5), 0.20)),
    ("窗口偏窄 x1.25 (拉伸)",       lambda im: squeeze(im, 1.25)),
    ("窗口偏宽 x0.80 (压缩)",       lambda im: squeeze(im, 0.80)),
    ("窗口偏宽 x0.65 (压缩)",       lambda im: squeeze(im, 0.65)),
]

def ev(fn):
    X = [fn(im) for im, _, _ in items]
    gt = np.array([i for _, i, _ in items])
    with torch.no_grad():
        pr = F.softmax(net(torch.stack([to_t(im) for im in X])), 1).numpy()
    pred, conf = pr.argmax(1), pr.max(1)
    r = {"acc": 100.0 * (pred == gt).mean(), "c50": float(np.median(conf))}
    for t in (0.70, 0.45):
        m = conf >= t
        r["cov%d" % (t * 100)] = 100.0 * m.mean()
        r["acc%d" % (t * 100)] = (100.0 * (pred[m] == gt[m]).mean()) if m.sum() else float("nan")
    return r

print("%-26s %6s %6s | %s | %s" % ("条件", "top1%", "置信中位", "0.70 覆盖/准确", "0.45 覆盖/准确"))
for name, fn in CASES:
    r = ev(fn)
    print("%-26s %6.1f %5.1f%% | %5.0f%%/%5.1f%% | %5.0f%%/%5.1f%%" % (
        name, r["acc"], 100 * r["c50"], r["cov70"], r["acc70"], r["cov45"], r["acc45"]))

print()
print("分省 top-1/置信中位 (只看崩了的):")
for name, fn in CASES:
    parts = []
    for ch in "京粤豫":
        sub = [(im, i) for im, i, c in items if c == ch]
        with torch.no_grad():
            pr = F.softmax(net(torch.stack([to_t(fn(im)) for im, _ in sub])), 1).numpy()
        gt = np.array([i for _, i in sub])
        parts.append("%s %3.0f%%/中位%3.0f%%" % (ch, 100.0 * (pr.argmax(1) == gt).mean(), 100 * np.median(pr.max(1))))
    print("  %-24s %s" % (name, "  ".join(parts)))
