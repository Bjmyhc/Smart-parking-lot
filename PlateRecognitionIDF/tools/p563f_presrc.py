# -*- coding: utf-8 -*-
"""p563f: 在 320x240 原图上先锐化、再缩到 94x24 —— 能不能让 r4 把省字读回"京"."""
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
net.load_state_dict(torch.load(os.path.join(ROOT,"tools","out","finetune","r4_lr1e4_nofreeze.pth"), map_location="cpu"))
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
def sat2(im, k256=512):
    f = im.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f-v)*k256)//256, 0, 255).astype(np.uint8)
def run(im):
    x = (sat2(im).astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(x.transpose(2,0,1)[None])).numpy()[0])
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def unsharp(im, k, a, sigma=0.0):
    g = cv2.GaussianBlur(im, (k, k), sigma)
    return cv2.addWeighted(im, 1 + a, g, -a, 0)
def to9424(roi):
    return cv2.resize(roi, (94, 24), interpolation=cv2.INTER_AREA)
fs = sorted([p for p in glob.glob(os.path.join(D,"IMG_20261001_2045*.jpg")) if load(p).shape[0] == 240])
lines = ["绿框内(缩 3px 去掉绿线) 直接重采样 -> 94x24 -> r4; # 真值 京Q06666", ""]
rows = []
for i, p in enumerate(fs):
    im = load(p)
    b, g, r = im[:,:,0].astype(int), im[:,:,1].astype(int), im[:,:,2].astype(int)
    m = (g > 150) & (r < 120) & (b < 120)
    ys, xs = np.nonzero(m)
    x0, x1, y0, y1 = xs.min()+3, xs.max()-3, ys.min()+3, ys.max()-3
    roi = im[y0:y1+1, x0:x1+1]
    outs = {}
    outs["原样"] = run(to9424(roi))
    for k, a in [(3, 0.6), (3, 1.0), (5, 1.0), (5, 1.6)]:
        outs["先锐%dx%.1f" % (k, a)] = run(to9424(unsharp(roi, k, a)))
    lines.append("FULL %d  " % i + "  ".join("%s:%-12s" % (k, v) for k, v in outs.items()))
    vis = cv2.resize(roi, (roi.shape[1]*3, roi.shape[0]*3), interpolation=cv2.INTER_NEAREST)
    rows.append(vis)
print("\n".join(lines))
open(os.path.join(OUT, "presrc_unsharp.txt"), "w", encoding="utf-8").write("\n".join(lines))
