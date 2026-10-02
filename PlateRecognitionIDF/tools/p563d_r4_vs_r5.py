# -*- coding: utf-8 -*-
"""p563d: r4 与 r5 对比 —— 老裁剪块(165 张) + 今天远距离 8 张."""
import os, sys, glob, collections
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
def load_net(ck):
    net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
    net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", ck), map_location="cpu"))
    net.eval(); return net
def dec(o):
    a = np.squeeze(np.asarray(o, dtype=np.float32))
    if a.shape[0] != len(CHARS): a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank: res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res)
def sat2(im, k256=512):
    f = im.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f-v)*k256)//256, 0, 255).astype(np.uint8)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def run(net, im):
    x = ((sat2(im).astype(np.float32)) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(x.transpose(2,0,1)[None])).numpy()[0])
nets = {}
for ck, name in [("r4_lr1e4_nofreeze.pth","r4"), ("r5_ccpd.pth","r5")]:
    pth = os.path.join(ROOT, "tools", "out", "finetune", ck)
    if os.path.isfile(pth): nets[name] = load_net(ck)
lines = ["模型: " + ", ".join(nets.keys()), ""]
lines.append("=== A) 今天(远距离)8 张真值 京Q06666 ===")
new = []
for p in sorted(glob.glob(os.path.join(D, "IMG_20261001_2045*.jpg"))):
    im = load(p)
    if im is not None and im.shape[0] == 24: new.append((os.path.basename(p)[-10:], im))
for nm, im in new:
    lines.append("%s  " % nm + "  ".join("%s:%-11s" % (k, run(v, im)) for k, v in nets.items()))
lines.append("")
lines.append("=== B) 老裁剪块(9/30) 回归 ===")
for folder in ["京Q-06666", "粤T-666FP", "豫J-7Z921", "豫A-12345"]:
    fs = []
    for p in sorted(glob.glob(os.path.join(D, folder, "*"))):
        im = load(p)
        if im is not None and im.shape[0] == 24 and im.shape[1] == 94: fs.append((p, im))
    truth = folder.replace("-", "")
    if not fs: continue
    for k, net in nets.items():
        res = [run(net, im) for _, im in fs]
        exact = sum(1 for r in res if r == truth)
        first = sum(1 for r in res if r[:1] == truth[:1])
        lines.append("%-10s n=%3d [%s] 整串全对 %3d/%d=%5.1f%%   首字对 %3d/%d=%5.1f%%" %
                     (folder, len(fs), k, exact, len(fs), 100.0*exact/len(fs), first, len(fs), 100.0*first/len(fs)))
open(os.path.join(OUT, "r4_vs_r5.txt"), "w", encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
