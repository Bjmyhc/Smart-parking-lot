# -*- coding: utf-8 -*-
"""p563c: 采样抖动(平移/缩放)能不能把"皖"翻回"京" —— 若能, 就是零训练的固件改法."""
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
def run(im):
    x = (im.astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(x.transpose(2,0,1)[None])).numpy()[0])
def sat2(im, k256=512):
    f = im.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f-v)*k256)//256, 0, 255).astype(np.uint8)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def warp(im, f, dx, dy):
    h, w = im.shape[:2]
    M = np.array([[f, 0, (1-f)*w/2 + dx], [0, f, (1-f)*h/2 + dy]], np.float32)
    return cv2.warpAffine(im, M, (w, h), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
fs = sorted([p for p in glob.glob(os.path.join(D,"IMG_20261001_2045*.jpg")) if load(p).shape[0]==24])
lines = ["真值 京Q06666 ; 记录每次抖动后模型吐的首字", ""]
tot_ok = 0
for p in fs:
    im = load(p); im = sat2(im)
    hits = []
    for f in (0.94, 1.0, 1.06):
        for dx in (-2,-1,0,1,2):
            for dy in (-1,0,1):
                r = run(warp(im, f, dx, dy))
                hits.append(r[:1] if r else "_")
    from collections import Counter
    c = Counter(hits)
    n_jing = c.get("京", 0)
    if n_jing: tot_ok += 1
    lines.append("%s  京 %2d/%2d  皖 %2d  | %s" % (os.path.basename(p)[-10:], n_jing, len(hits), c.get("皖",0),
                 " ".join("%sx%d" % (k,v) for k,v in c.most_common(6))))
lines.append("")
lines.append("有'京'出现的裁剪块: %d/%d" % (tot_ok, len(fs)))
open(os.path.join(OUT,"jitter.txt"),"w",encoding="utf-8").write("\n".join(lines))
print("\n".join(lines))
