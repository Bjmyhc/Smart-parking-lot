# -*- coding: utf-8 -*-
"""p563_crop_probe.py -- 拿助手存下来的 94x24 裁剪块, 复算固件的预处理链, 看模型到底读到什么.
变体:
  A 原样 (直接量化喂模型, 不做饱和度增强)
  B 固件链 (饱和度 x2.0 后再喂)  <- 板上真正喂给模型的就是这个
"""
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

def dec(out):
    a = np.squeeze(np.asarray(out, dtype=np.float32))
    if a.shape[0] != len(CHARS): a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank: res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res), a

def sat2(im, k256=512):
    f = im.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)

def run(im):
    x = (im.astype(np.float32) - 127.5) * 0.0078125
    o = net(torch.from_numpy(x.transpose(2, 0, 1)[None])).numpy()[0]
    return dec(o)

def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)

fs = sorted(glob.glob(os.path.join(D, "IMG_20261001_2045*.jpg")))
fs = [p for p in fs if load(p).shape[0] == 24]
lines = ["file | A_原样 | B_固件链(饱和x2)", "-"*60]
for p in fs:
    im = load(p)
    a, pa = run(im)
    b, pb = run(sat2(im))
    # 第一步的 top2
    def top2(a):
        col = a[:, 1]
        idx = np.argsort(-col)[:2]
        return " ".join("%s%.0f%%" % (CHARS[i], col[i]*100) for i in idx)
    lines.append("%s | %-12s %s | %-12s %s" % (os.path.basename(p)[-10:], a, top2(pa), b, top2(pb)))
open(os.path.join(OUT, "probe.txt"), "w", encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
