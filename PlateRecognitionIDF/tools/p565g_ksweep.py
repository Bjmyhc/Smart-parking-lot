# -*- coding: utf-8 -*-
"""p565g: 合成预训练 + k 张/省真实微调 -> 留出真实集准确率. 回答"每省要拍几张"."""
import os, sys, glob, random
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F
sys.path.insert(0, r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools")
import importlib.util
spec = importlib.util.spec_from_file_location("g", r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\p565f_train2.py")
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
# 复用 p565f 的渲染/增广/模型 (它 import 时会跑训练, 所以这里手工把需要的函数抄进来: 用 exec 只取定义)
src = open(os.path.join(ROOT, "tools", "p565f_train2.py"), encoding="utf-8").read()
cut = src.index("real = []")
ns = {}
exec(src[:cut], ns)
render, augment, Net, to_t, PROV, W, H = ns["render"], ns["augment"], ns["Net"], ns["to_t"], ns["PROV"], ns["W"], ns["H"]
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
real = {}
for ch, lab in [("京","jing"),("粤","yue"),("豫","yu")]:
    ps = sorted(glob.glob(os.path.join(OUT,"real_all",lab,"*.png")))
    real[ch] = [(load(p), PROV.index(ch)) for p in ps]
pre = os.path.join(OUT, "prov_net_synth2.pth")
log = []
for K in (8, 16, 32, 64):
    tr, va = [], []
    for ch, items in real.items():
        random.Random(7).shuffle(items)
        tr += items[:K]; va += items[K:]
    net = Net(); net.load_state_dict(torch.load(pre, map_location="cpu"))
    for m in net.modules():
        if isinstance(m, nn.BatchNorm2d): m.momentum = 0.05
    opt = torch.optim.Adam(net.parameters(), lr=4e-4, weight_decay=1e-5)
    rng = random.Random(99)
    REP = max(1, 400 // max(1, K))
    for ep in range(8):
        net.train()
        X, Y = [], []
        for i, ch in enumerate(PROV):
            for _ in range(120):
                X.append(to_t(augment(render(ch, rng), rng))); Y.append(i)
        for im, i in tr*REP:
            X.append(to_t(augment(im, rng) if rng.random() < 0.5 else im)); Y.append(i)
        idx = torch.randperm(len(X))
        for b in range(0, len(X), 64):
            sel = idx[b:b+64]
            opt.zero_grad()
            loss = F.cross_entropy(net(torch.stack([X[j] for j in sel])), torch.tensor([Y[j] for j in sel]))
            loss.backward(); opt.step()
    net.eval()
    with torch.no_grad():
        pr = net(torch.stack([to_t(im) for im,_ in va])).argmax(1).numpy()
    ok = sum(1 for (im,i),p in zip(va,pr) if p == i)
    # 只在这三省里排名(排除其他 28 类干扰) -> 更贴近"主模型已给出候选"的场景
    with torch.no_grad():
        lg = net(torch.stack([to_t(im) for im,_ in va]))
        idx3 = [PROV.index(c) for c in real]
        sub = lg[:, idx3]
        p2 = sub.argmax(1).numpy()
    ok2 = sum(1 for (im,i),p in zip(va,p2) if idx3[p] == i)
    line = "每省 %2d 张真实 -> 留出 %3d 张: 31 类里对 %3d (%5.1f%%) | 只在这 3 省里对 %3d (%5.1f%%)" % \
           (K, len(va), ok, 100.0*ok/len(va), ok2, 100.0*ok2/len(va))
    print(line); log.append(line)
open(os.path.join(OUT,"k_sweep.txt"),"w",encoding="utf-8").write("\n".join(log))
