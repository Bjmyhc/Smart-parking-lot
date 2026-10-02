# -*- coding: utf-8 -*-
"""p569: 定稿版省字模型 (平均池化单支) —— 全部真实素材微调 + 导出 ONNX.

选它不是因为"简单", 是消融实测最好:
    avg 96.7% / max 93.4% / avg+max 93.4%  (留出 61 张真实省字, 见 p568_ablation.txt)
而且 avg 单支不带 Concat -> 量化不会插入 RequantizeLinear -> 没有 0 维标量参数
(0 维参数会让板端 fbs_model.cpp:649 解引用空指针, 前一轮已实测崩溃).

产物: out/p565/prov_final_avg.pth, out/p565/prov_final.onnx (给 p567 量化用)
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
    """backbone 同 p565f 的 Net; 头部只用 GlobalAveragePool 那一支."""
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

real = []
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            real.append((im, PROV.index(ch)))
print("全部真实素材 %d 张参与微调" % len(real))

net = warm_start()
for m in net.modules():
    if isinstance(m, nn.BatchNorm2d):
        m.momentum = 0.05
opt = torch.optim.Adam(net.parameters(), lr=4e-4, weight_decay=1e-5)
rng = random.Random(99)
REP = max(1, int(round(1200 / len(real))))
for ep in range(12):
    net.train()
    X, Y = [], []
    for i, ch in enumerate(PROV):
        for _ in range(120):
            X.append(to_t(augment(render(ch, rng), rng))); Y.append(i)
    for _ in range(REP):
        for im, i in real:
            X.append(to_t(augment(im, rng) if rng.random() < 0.6 else im)); Y.append(i)
    idx = torch.randperm(len(X))
    tot = 0.0
    for b in range(0, len(X), 64):
        sel = idx[b:b + 64]
        opt.zero_grad()
        loss = F.cross_entropy(net(torch.stack([X[j] for j in sel])), torch.tensor([Y[j] for j in sel]))
        loss.backward(); opt.step(); tot += float(loss)
    print("ep %2d loss %.4f" % (ep + 1, tot / max(1, len(X) // 64)))
net.eval()
with torch.no_grad():
    pr = net(torch.stack([to_t(im) for im, _ in real])).argmax(1).numpy()
gt = np.array([i for _, i in real])
print("训练集自身 %.1f%% (%d/%d)  [乐观数字]" % (100.0 * (pr == gt).mean(), int((pr == gt).sum()), len(gt)))
torch.save(net.state_dict(), os.path.join(OUT, "prov_final_avg.pth"))

def fold_bn(net):
    layers = []
    for m in net.f:
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
    def __init__(self, src_net):
        super().__init__()
        self.f = fold_bn(src_net)
        self.head = nn.Linear(64, 31)
        self.head.weight.data = src_net.head.weight.data.clone()
        self.head.bias.data = src_net.head.bias.data.clone()

    def forward(self, x):
        return self.head(F.adaptive_avg_pool2d(self.f(x), 1).flatten(1))

exp = ProvExport(net).eval()
dummy = torch.randn(1, 3, H, W)
onnx_path = os.path.join(OUT, "prov_final.onnx")
try:
    torch.onnx.export(exp, dummy, onnx_path, dynamo=False, input_names=["input"], output_names=["output"], opset_version=13)
except TypeError:
    torch.onnx.export(exp, dummy, onnx_path, input_names=["input"], output_names=["output"], opset_version=13)
print("ONNX -> %s (%.1f KB)" % (onnx_path, os.path.getsize(onnx_path) / 1024.0))
import onnx, collections, sys
mdl = onnx.load(onnx_path); onnx.checker.check_model(mdl)
ops = collections.Counter(n.op_type for n in mdl.graph.node)
print("算子: %s" % dict(sorted(ops.items())))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import espdl_ops
missing, _ = espdl_ops.check(ops)
print("esp-dl 算子体检: %s" % ("通过" if not missing else "缺 %s" % missing))
import onnxruntime as ort
sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
b = sess.run(None, {sess.get_inputs()[0].name: dummy.numpy()})[0]
with torch.no_grad():
    a = exp(dummy).numpy()
print("PyTorch vs ONNX 最大绝对差 %.2e" % float(np.abs(a - b).max()))