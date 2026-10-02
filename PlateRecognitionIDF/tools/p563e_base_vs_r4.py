# -*- coding: utf-8 -*-
"""p563e: 三份权重对比 —— 原始基线 Final_LPRNet_model.pth / r4(现役) / 是否有 r5."""
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
def netof(path):
    n = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
    n.load_state_dict(torch.load(path, map_location="cpu")); n.eval(); return n
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
    x = (sat2(im).astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(x.transpose(2,0,1)[None])).numpy()[0])
nets = {"base": netof(os.path.join(LPRNET, "weights", "Final_LPRNet_model.pth"))}
f4 = os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth")
if os.path.isfile(f4): nets["r4"] = netof(f4)
f5 = os.path.join(ROOT, "tools", "out", "finetune", "r5_ccpd.pth")
if os.path.isfile(f5): nets["r5"] = netof(f5)
lines = ["模型: " + ", ".join(nets.keys()), "", "=== A) 今天远距离 8 张 (真值 京Q06666) ==="]
new = [(os.path.basename(p)[-10:], load(p)) for p in sorted(glob.glob(os.path.join(D,"IMG_20261001_2045*.jpg")))]
new = [(n, im) for n, im in new if im is not None and im.shape[0] == 24]
for nm, im in new:
    lines.append("%s  " % nm + "   ".join("%s:%-12s" % (k, run(v, im)) for k, v in nets.items()))
lines += ["", "=== B) 老裁剪块回归 (整串全对 / 首字对) ==="]
for folder in ["京Q-06666", "粤T-666FP", "豫J-7Z921", "豫A-12345"]:
    fs = [(p, load(p)) for p in sorted(glob.glob(os.path.join(D, folder, "*")))]
    fs = [(p, im) for p, im in fs if im is not None and im.shape == (24, 94, 3)]
    truth = folder.replace("-", "")
    if not fs: continue
    row = []
    for k, net in nets.items():
        res = [run(net, im) for _, im in fs]
        ex = sum(1 for r in res if r == truth); fi = sum(1 for r in res if r[:1] == truth[:1])
        row.append("%s %2d/%2d(%3.0f%%) 首%2d/%2d(%3.0f%%)" % (k, ex, len(fs), 100.0*ex/len(fs), fi, len(fs), 100.0*fi/len(fs)))
    lines.append("%-10s n=%3d | %s" % (folder, len(fs), " | ".join(row)))
open(os.path.join(OUT, "base_vs_r4.txt"), "w", encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
