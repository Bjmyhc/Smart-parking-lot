# -*- coding: utf-8 -*-
"""p566: 省字模型定稿 —— 合成预训练 + 全部真实素材微调, 产出可量化/可上板的省字分类器.

沿用 p565g 已验证的配方 (每省 16 张 -> 留出集 90.9%), 但把真实样本用满:
  阶段A: 留出 25% 真实样本 -> 只训其余 75%, 报"留出集准确率"(这是唯一诚实的数字)
  阶段B: 全部真实样本参与微调 -> prov_final.pth (上板用这个)
  再导出 ONNX: BN 折进 conv, 池化只用 esp-dl 注册表里的算子 (MaxPool/GlobalAveragePool)

产物 (tools/out/p565/):
  prov_final.pth / prov_final.onnx / p566_report.txt
"""
import os, glob, random, sys
import numpy as np, cv2, torch, torch.nn as nn, torch.nn.functional as F

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
OUT = os.path.join(ROOT, "tools", "out", "p565")
PRE = os.path.join(OUT, "prov_net_synth2.pth")          # p565f 的合成预训练权重
REP_TXT = os.path.join(OUT, "p566_report.txt")

# 复用 p565f 的渲染/增广/网络定义 (它 import 时会直接跑训练, 所以只 exec 到 "real = []" 之前)
src = open(os.path.join(ROOT, "tools", "p565f_train2.py"), encoding="utf-8").read()
ns = {}
exec(src[:src.index("real = []")], ns)
render, augment, Net, to_t, PROV, W, H = ns["render"], ns["augment"], ns["Net"], ns["to_t"], ns["PROV"], ns["W"], ns["H"]

lines = []
def say(s):
    print(s); lines.append(s)

def load(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

# ---- 真实样本: 京 146 / 粤 32 / 豫 67 = 245 ----
real = {}
for ch, lab in [("京", "jing"), ("粤", "yue"), ("豫", "yu")]:
    items = []
    for p in sorted(glob.glob(os.path.join(OUT, "real_all", lab, "*.png"))):
        im = load(p)
        if im is not None:
            items.append((im, PROV.index(ch)))
    real[ch] = items
say("真实素材: " + "  ".join("%s %d" % (c, len(v)) for c, v in real.items()) + "  合计 %d" % sum(len(v) for v in real.values()))

def finetune(tr, epochs=10, rep_target=1200, seed=99, lr=4e-4, aug_p=0.6):
    """合成(31类 x120/轮) + 真实样本(每轮重复 REP 遍) 混合微调 —— 与 p565g 同配方."""
    net = Net()
    net.load_state_dict(torch.load(PRE, map_location="cpu"))
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
    return net, REP

def probs_of(net, items):
    with torch.no_grad():
        lg = net(torch.stack([to_t(im) for im, _ in items]))
        pr = F.softmax(lg, 1).numpy()
    return lg.numpy(), pr

# ---- 阶段A: 留出集 ----
say("")
say("=" * 74)
say("阶段A: 留出 25% 真实样本, 只用其余 75% 微调 (留出集数字才算数)")
say("=" * 74)
tr, va = [], []
for ch, items in real.items():
    it = list(items); random.Random(7).shuffle(it)
    k = max(4, int(round(len(it) * 0.25)))
    va += it[:k]; tr += it[k:]
say("训练 %d 张 / 留出 %d 张" % (len(tr), len(va)))
netA, REP = finetune(tr)
say("真实样本每轮重复 %d 遍" % REP)
lg, pr = probs_of(netA, va)
pred = pr.argmax(1)
ok = int((pred == np.array([i for _, i in va])).sum())
say("留出集 top-1: %.1f%% (%d/%d)" % (100.0 * ok / len(va), ok, len(va)))
per = {}
for (im, i), p in zip(va, pred):
    c = PROV[i]; per.setdefault(c, [0, 0]); per[c][1] += 1; per[c][0] += int(p == i)
say("分省: " + "  ".join("%s %d/%d" % (c, v[0], v[1]) for c, v in per.items()))

# 置信度-准确率曲线 (决定板上"信到多少才敢覆盖主模型")
say("")
say("置信度门槛扫描 (留出集): 门槛t 以上才采纳省字, 以下交回主模型/不报")
say("  %-8s %-10s %-10s" % ("门槛", "覆盖率", "采纳后准确率"))
conf = pr.max(1)
for t in (0.40, 0.50, 0.60, 0.70, 0.80, 0.90):
    m = conf >= t
    if m.sum() == 0:
        say("  %-8.2f %-10s %-10s" % (t, "0%", "-")); continue
    a = float((pred[m] == np.array([i for _, i in va])[m]).mean())
    say("  %-8.2f %-9.0f%% %-9.0f%%" % (t, 100.0 * m.mean(), 100.0 * a))
# 错误样本的置信度 -> 看"自信地错"有多严重
wrong = pred != np.array([i for _, i in va])
if wrong.sum():
    say("错判 %d 张, 它们的置信度: 最高 %.2f 中位 %.2f" % (int(wrong.sum()), float(conf[wrong].max()), float(np.median(conf[wrong]))))
else:
    say("留出集全对")

# ---- 阶段B: 全部真实样本 ----
say("")
say("=" * 74)
say("阶段B: 全部真实样本参与微调 -> prov_final.pth (上板用)")
say("=" * 74)
netB, REP2 = finetune([it for v in real.values() for it in v])
say("真实样本每轮重复 %d 遍" % REP2)
lgb, prb = probs_of(netB, [it for v in real.values() for it in v])
okb = int((prb.argmax(1) == np.array([i for v in real.values() for _, i in v])).sum())
n_all = sum(len(v) for v in real.values())
say("训练集自身准确率 %.1f%% (%d/%d)  [乐观数字, 只反映拟合程度, 不是泛化指标]" % (100.0 * okb / n_all, okb, n_all))
torch.save(netB.state_dict(), os.path.join(OUT, "prov_final.pth"))
say("权重 -> %s" % os.path.join(OUT, "prov_final.pth"))

# ---- 导出 ONNX ----
say("")
say("=" * 74)
say("导出 ONNX (BN 折进 conv; 全局池化只用 MaxPool/GlobalAveragePool)")
say("=" * 74)

def fold_bn(net):
    """把 BN 折进前一层 conv, 得到等价的无 BN 网络 (esp-dl 运行端没有 BatchNormalization)."""
    layers = []
    for m in net.f:
        if isinstance(m, nn.Sequential):          # conv + bn + relu
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
    """板上等价网络: 输入 [1,3,64,32] (NCHW, 已归一化), 输出 [1,31] logits."""
    def __init__(self, net):
        super().__init__()
        self.f = fold_bn(net)
        self.head = nn.Linear(128, 31)
        self.head.weight.data = net.head.weight.data.clone()
        self.head.bias.data = net.head.bias.data.clone()
        # 全局最大池化的核尺寸 = 这一层的特征图尺寸, 在导出前用一次前向量出来当常量,
        # 免得 torch 追踪时去 int(shape) 报错 (GlobalMaxPool 运行端没注册, 必须写成 MaxPool)
        with torch.no_grad():
            y = self.f(torch.zeros(1, 3, H, W))
        self.kh, self.kw = int(y.shape[2]), int(y.shape[3])

    def forward(self, x):
        y = self.f(x)
        avg = F.adaptive_avg_pool2d(y, 1).flatten(1)                  # GlobalAveragePool
        mx = F.max_pool2d(y, (self.kh, self.kw)).flatten(1)            # MaxPool (核=整幅)
        return self.head(torch.cat([avg, mx], 1))

exp = ProvExport(netB).eval()
dummy = torch.randn(1, 3, H, W)
onnx_path = os.path.join(OUT, "prov_final.onnx")
try:   # torch 2.14 的默认导出器 (dynamo) 要 onnxscript, 本机没装 -> 走老的 TorchScript 导出器
    torch.onnx.export(exp, dummy, onnx_path, dynamo=False,
                      input_names=["input"], output_names=["output"], opset_version=13)
except TypeError:
    torch.onnx.export(exp, dummy, onnx_path,
                      input_names=["input"], output_names=["output"], opset_version=13)
say("ONNX -> %s (%.1f KB)" % (onnx_path, os.path.getsize(onnx_path) / 1024.0))
import onnx, collections
mdl = onnx.load(onnx_path); onnx.checker.check_model(mdl)
ops = collections.Counter(n.op_type for n in mdl.graph.node)
say("算子: %s" % dict(sorted(ops.items())))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import espdl_ops
missing, supported = espdl_ops.check(ops)
say("esp-dl 算子体检: %s" % ("通过" if not missing else "缺 %s" % missing))
# 数值一致性: 折叠前后 + onnxruntime
with torch.no_grad():
    a = exp(dummy).numpy()
import onnxruntime as ort
sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
b = sess.run(None, {sess.get_inputs()[0].name: dummy.numpy()})[0]
say("PyTorch vs ONNX 最大绝对差 %.2e" % float(np.abs(a - b).max()))
opt = ort.SessionOptions(); opt.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
sess2 = ort.InferenceSession(onnx_path, opt, providers=["CPUExecutionProvider"])
b2 = sess2.run(None, {sess2.get_inputs()[0].name: dummy.numpy()})[0]
say("关掉图优化后 最大绝对差 %.2e (量化前一眼看出有没有折叠错)" % float(np.abs(a - b2).max()))

open(REP_TXT, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("报告 ->", REP_TXT)