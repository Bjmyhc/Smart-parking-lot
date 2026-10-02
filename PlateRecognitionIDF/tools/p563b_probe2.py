# -*- coding: utf-8 -*-
"""p563b: 省字为什么糊了就变"皖" —— 多预处理对照 + 新老裁剪块对比."""
import os, sys, glob
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT = os.path.join(ROOT, "tools", "out", "p563"); os.makedirs(OUT, exist_ok=True)
blank = len(CHARS) - 1
torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth"),
                               map_location="cpu"))
net.eval()
def dec(o):
    a = np.squeeze(np.asarray(o, dtype=np.float32))
    if a.shape[0] != len(CHARS): a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank: res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res)
def run(im):
    x = (im.astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(x.transpose(2,0,1)[None])).numpy()[0])
def sat2(im, k256=512):
    f = im.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)
def unsharp(im, k=3, a=1.0):
    g = cv2.GaussianBlur(im, (k, k), 0)
    return cv2.addWeighted(im, 1 + a, g, -a, 0)
def sharp_of(im):
    gray = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
    return float(np.abs(cv2.Laplacian(gray, cv2.CV_32F)).mean())
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def collect(folder):
    out = []
    for p in sorted(glob.glob(os.path.join(D, folder, "*"))):
        im = load(p)
        if im is not None and im.shape[0] == 24 and im.shape[1] == 94: out.append((os.path.basename(p), im))
    return out
groups = [("NEW 远(20:45)", [os.path.basename(p) for p in sorted(glob.glob(os.path.join(D,"IMG_20261001_2045*.jpg")))], None)]
newset = []
for p in sorted(glob.glob(os.path.join(D, "IMG_20261001_2045*.jpg"))):
    im = load(p)
    if im is not None and im.shape[0] == 24: newset.append((os.path.basename(p)[-10:], im))
OLD = {f: collect(f) for f in ["京Q-06666", "粤T-666FP", "豫J-7Z921", "豫A-12345"]}
lines = []
lines.append("=== 1) 新(远)裁剪块: 各预处理下模型读到什么 (真值 京Q06666) ===")
for name, im in newset:
    r0 = run(im); r1 = run(sat2(im))
    r2 = run(unsharp(sat2(im), 3, 1.0)); r3 = run(unsharp(sat2(im), 3, 2.0))
    r4 = run(unsharp(sat2(im), 5, 1.5))
    lines.append("%s sharp=%5.1f | 原样:%-11s 饱和x2:%-11s +锐1.0:%-11s +锐2.0:%-11s +锐5/1.5:%-11s" %
                 (name, sharp_of(im), r0, r1, r2, r3, r4))
lines.append("")
lines.append("=== 2) 老裁剪块(9/30 存的) 现在读成什么, 以及清晰度 ===")
for f, items in OLD.items():
    if not items: continue
    res = [run(sat2(im)) for _, im in items]
    sharp = [sharp_of(im) for _, im in items]
    from collections import Counter
    c = Counter(res)
    lines.append("%-10s n=%3d sharp 中位 %5.1f | top: %s" % (f, len(items), float(np.median(sharp)),
                 ", ".join("%s x%d" % (k, v) for k, v in c.most_common(4))))
lines.append("")
lines.append("=== 3) 新(远)块 均值/锐度 ===")
for name, im in newset:
    b, g, r = im.reshape(-1,3).mean(axis=0)
    lines.append("%s 均BGR %5.1f/%5.1f/%5.1f  sharp %5.1f" % (name, b, g, r, sharp_of(im)))
open(os.path.join(OUT,"probe2.txt"),"w",encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
