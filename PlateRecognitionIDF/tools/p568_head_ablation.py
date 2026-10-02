# -*- coding: utf-8 -*-
"""p568: 头部消融 —— 省字模型最后一步池化用 平均/最大/两者拼接 哪个好?

为什么必须做: 量化后 esp-dl 的 Concat 要求两支尺度一致, esp-ppq 会插一个
RequantizeLinear, 并把它需要的 scale/zero_point 写成 **0 维标量 initializer**。
运行端 fbs_model.cpp:649 读参数 dims 时会解引用空指针 (LoadProhibited, 前一轮已实测)。
所以能不带 Concat 就不带 —— 先看单支(平均)会不会掉精度, 掉了多少, 再决定值不值得冒这个险。

用法: python p568_head_ablation.py
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

def load(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

class NetH(nn.Module):
    """backbone 与 p565f 的 Net 逐层一致; 只换最后一步池化 (mode)."""
    def __init__(self, mode, n=31):
        super().__init__()
        def blk(i, o):
            return nn.Sequential(nn.Conv2d(i, o, 3, padding=1, bias=False), nn.BatchNorm2d(o), nn.ReLU(inplace=True))
        self.f = nn.Sequential(blk(3, 16), nn.MaxPool2d(2), blk(16, 32), nn.MaxPool2d(2),
                               blk(32, 64), nn.MaxPool2d(2), blk(64, 64))
        self.mode = mode
        self.head = nn.Linear(64 if mode != "both" else 128, n)

    def forward(self, x):
        y = self.f(x)
        feats = []
        if self.mode in ("avg", "both"):
            feats.append(F.adaptive_avg_pool2d(y, 1).flatten(1))
        if self.mode in ("max", "both"):
            feats.append(F.max_pool2d(y, (y.shape[2], y.shape[3])).flatten(1))
        return self.head(feats[0] if len(feats) == 1 else torch.cat(feats, 1))

def warm_start(mode):
    """从 p565f 的 both 头里切出对应的一半 —— 比随机初始化强得多."""
    sd = torch.load(PRE, map_location="cpu")
    net = NetH(mode)
    bb = {k: v for k, v in sd.items() if not k.startswith("head.")}
    net.load_state_dict({**net.state_dict(), **bb}, strict=False)
    with torch.no_grad():
        if mode == "avg":
            net.head.weight.copy_(sd["head.weight"][:, :64]); net.head.bias.copy_(sd["head.bias"])
        elif mode == "max":
            net.head.weight.copy_(sd["head.weight"][:, 64:]); net.head.bias.copy_(sd["head.bias"])
        else:
            net.head.weight.copy_(sd["head.weight"]); net.head.bias.copy_(sd["head.bias"])
    return net

def finetune(mode, tr, epochs=10, rep_target=1200, seed=99, lr=4e-4, aug_p=0.6):
    net = warm_start(mode)
    for m in net.modules():
        if isinstance(m, nn.BatchNorm2d):
            m.momentum = 0.05
    opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=1e-5)
    rng = random.Random(seed)
    REP = max(1, int(round(rep_target / max(1, len(tr)))))
    for ep in range(epochs):
        net.train()
        X, Y = [], []
        for i, ch in enumerate(PROV):
            for _ in range(120):
                X.append(to_t(augment(render(ch, rng), rng))); Y.append(i)
        for _ in range(REP):
            for im, i in tr:
                X.append(to_t(augment(im, rng) if rng.random() < aug_p else im)); Y.append(i)
        idx = torch.randperm(len(X))
        for b in range(0, len(X), 64):
            sel = idx[b:b + 64]
            opt.zero_grad()
            loss = F.cross_entropy(net(torch.stack([X[j] for j in sel])), torch.tensor([Y[j] for j in sel]))
            loss.backward(); opt.step()
    net.eval()
    return net

real = {}
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    items = []
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            items.append((im, PROV.index(ch)))
    real[ch] = items
tr, va = [], []
for ch, items in real.items():
    it = list(items); random.Random(7).shuffle(it)
    k = max(4, int(round(len(it) * 0.25)))
    va += it[:k]; tr += it[k:]
gt = np.array([i for _, i in va])
print("训练 %d / 留出 %d" % (len(tr), len(va)))
lines = []
for mode in ("avg", "max", "both"):
    net = finetune(mode, tr)
    with torch.no_grad():
        lg = net(torch.stack([to_t(im) for im, _ in va]))
        pr = F.softmax(lg, 1).numpy()
    pred = pr.argmax(1)
    ok = int((pred == gt).sum())
    # 只看可用样本(置信度 >= 0.70)时的准确率
    m = pr.max(1) >= 0.70
    okt = int((pred[m] == gt[m]).sum())
    line = "mode=%-5s 留出 top-1 %5.1f%% (%d/%d) | 置信>=0.70 时 %5.1f%% (%d/%d, 覆盖 %.0f%%)" % (
        mode, 100.0 * ok / len(va), ok, len(va), 100.0 * okt / max(1, m.sum()), okt, int(m.sum()), 100.0 * m.mean())
    print(line); lines.append(line)
open(os.path.join(OUT, "p568_ablation.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")