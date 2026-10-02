# -*- coding: utf-8 -*-
"""p570: 省字模型 4 折交叉验证 (诚实的泛化数字) + 用全部数据出上板权重.

为什么不用单一留出: 245 张素材, 单次留出只剩 61 张, 一个样本就动 1.6 个百分点.
4 折 = 244 次评估 (等于把素材用 4 遍), 报的均值才是可信的泛化估计.
所有折都用同一套配方 (合成 31x120/轮 + 真实每轮 5 遍, 10 轮), 与 p568 消融里
avg 单支拿到 96.7% 的那次逐参数一致.

产物: out/p565/prov_final_avg.pth + prov_final.onnx (上板用), out/p565/p570_kfold.txt
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

def warm_start():
    sd = torch.load(PRE, map_location="cpu")
    net = NetA()
    net.load_state_dict({k: v for k, v in sd.items() if not k.startswith("head.")}, strict=False)
    with torch.no_grad():
        net.head.weight.copy_(sd["head.weight"][:, :64])
        net.head.bias.copy_(sd["head.bias"])
    return net

def finetune(tr, epochs=10, seed=99, lr=4e-4, aug_p=0.6):
    net = warm_start()
    for m in net.modules():
        if isinstance(m, nn.BatchNorm2d):
            m.momentum = 0.05
    opt = torch.optim.Adam(net.parameters(), lr=lr, weight_decay=1e-5)
    rng = random.Random(seed)
    REP = max(1, int(round(1200 / max(1, len(tr)))))
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

def evaluate(net, va):
    gt = np.array([i for _, i in va])
    with torch.no_grad():
        pr = F.softmax(net(torch.stack([to_t(im) for im, _ in va])), 1).numpy()
    pred = pr.argmax(1); conf = pr.max(1)
    m = conf >= 0.70
    return (int((pred == gt).sum()), len(gt), int((pred[m] == gt[m]).sum()) if m.sum() else 0, int(m.sum()))

real = {}
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    items = []
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            items.append((im, PROV.index(ch)))
    real[ch] = items

NF = 4
folds = {}
for ch, items in real.items():
    it = list(items); random.Random(7).shuffle(it)
    folds[ch] = [it[i::NF] for i in range(NF)]      # 交错切, 每折大小接近

lines = []
tot_ok = tot_n = 0
ok7 = n7 = 0
for f in range(NF):
    tr = [x for ch in real for i in range(NF) if i != f for x in folds[ch][i]]
    va = [x for ch in real for x in folds[ch][f]]
    net = finetune(tr)
    ok, n, o7, n7f = evaluate(net, va)
    per = {}
    for (im, i) in va:
        pass
    gt = np.array([i for _, i in va])
    with torch.no_grad():
        pred = net(torch.stack([to_t(im) for im, _ in va])).argmax(1).numpy()
    for (im, i), p in zip(va, pred):
        per.setdefault(PROV[i], [0, 0]); per[PROV[i]][1] += 1; per[PROV[i]][0] += int(p == i)
    line = "折%d: 训练 %3d 留出 %3d -> %5.1f%% (%d/%d) | 置信>=0.70: %5.1f%% (%d/%d, 覆盖 %.0f%%) | %s" % (
        f + 1, len(tr), n, 100.0 * ok / n, ok, n, 100.0 * o7 / max(1, n7f), o7, n7f, 100.0 * n7f / n,
        " ".join("%s %d/%d" % (c, v[0], v[1]) for c, v in per.items()))
    print(line); lines.append(line)
    tot_ok += ok; tot_n += n; ok7 += o7; n7 += n7f

line = "4 折合计 %d 张: top-1 %5.1f%% (%d/%d) | 置信>=0.70 时 %5.1f%% (%d/%d, 覆盖 %.0f%%)" % (
    tot_n, 100.0 * tot_ok / tot_n, tot_ok, tot_n, 100.0 * ok7 / max(1, n7), ok7, n7, 100.0 * n7 / tot_n)
print(line); lines.append(line)

# ---- 出上板权重: 全部 245 张 ----
net = finetune([x for ch in real for x in real[ch]])
torch.save(net.state_dict(), os.path.join(OUT, "prov_final_avg.pth"))
ok, n, o7, n7f = evaluate(net, [x for ch in real for x in real[ch]])
line = "全量微调权重(上板): 训练集自身 %5.1f%% (%d/%d) | 置信>=0.70 时 %5.1f%% (覆盖 %.0f%%)" % (
    100.0 * ok / n, ok, n, 100.0 * o7 / max(1, n7f), 100.0 * n7f / n)
print(line); lines.append(line)

def fold_bn(n):
    layers = []
    for m in n.f:
        if isinstance(m, nn.Sequential):
            conv, bn, relu = m[0], m[1], m[2]
            s = bn.weight / torch.sqrt(bn.running_var + bn.eps)
            nc = nn.Conv2d(conv.in_channels, conv.out_channels, 3, padding=1, bias=True)
            nc.weight.data = conv.weight.data * s[:, None, None, None]
            nc.bias.data = bn.bias.data - bn.running_mean * s
            layers += [nc, nn.ReLU(inplace=True)]
        else:
            layers.append(m)
    return nn.Sequential(*layers)

class ProvExport(nn.Module):
    def __init__(self, s):
        super().__init__()
        self.f = fold_bn(s)
        self.head = nn.Linear(64, 31)
        self.head.weight.data = s.head.weight.data.clone()
        self.head.bias.data = s.head.bias.data.clone()

    def forward(self, x):
        return self.head(F.adaptive_avg_pool2d(self.f(x), 1).flatten(1))

exp = ProvExport(net).eval()
dummy = torch.randn(1, 3, H, W)
p = os.path.join(OUT, "prov_final.onnx")
try:
    torch.onnx.export(exp, dummy, p, dynamo=False, input_names=["input"], output_names=["output"], opset_version=13)
except TypeError:
    torch.onnx.export(exp, dummy, p, input_names=["input"], output_names=["output"], opset_version=13)
import onnx, collections, sys
mdl = onnx.load(p); onnx.checker.check_model(mdl)
ops = collections.Counter(n.op_type for n in mdl.graph.node)
line = "ONNX %.1f KB 算子: %s" % (os.path.getsize(p) / 1024.0, dict(sorted(ops.items())))
print(line); lines.append(line)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import espdl_ops
missing, _ = espdl_ops.check(ops)
line = "esp-dl 算子体检: %s" % ("通过" if not missing else "缺 %s" % missing)
print(line); lines.append(line)
open(os.path.join(OUT, "p570_kfold.txt"), "w", encoding="utf-8").write("\n".join(lines) + "\n")