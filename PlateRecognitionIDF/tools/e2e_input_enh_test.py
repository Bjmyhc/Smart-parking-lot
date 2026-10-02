# -*- coding: utf-8 -*-
"""拿 175 张真实 94x24 模型输入块, 独立验证"增强模型输入"到底帮忙还是帮倒忙。"""
import os, sys, glob
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
import locator_text_row as T
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
blank = len(CHARS) - 1
def dec(out):
    a = np.squeeze(np.asarray(out, dtype=np.float32))
    if a.shape[0] != len(CHARS): a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank: res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res)
torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth"), map_location="cpu"))
net.eval()
def infer(img):
    im = cv2.resize(img, (94, 24)).astype(np.float32)
    im = (im - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2,0,1)[None])).numpy()[0])

BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
items = []
for d, gt in (("京Q-06666", "京Q06666"), ("粤T-666FP", "粤T666FP"),
              ("豫A-12345", "豫A12345"), ("豫J-7Z921", "豫J7Z921")):
    for f in sorted(glob.glob(os.path.join(BASE, d, "*.jpg"))): items.append((f, gt))
for f in sorted(glob.glob(os.path.join(BASE, "IMG_20261001_1740*.jpg"))): items.append((f, "京Q06666"))

variants = [("原样", lambda im: im),
            ("CLAHE+饱和", lambda im: T.preprocess(im, "clahe+sat")),
            ("只饱和", lambda im: T.preprocess(im, "sat")),
            ("只CLAHE", lambda im: T.preprocess(im, "clahe"))]
res = {n: {"full": 0, "prov": 0, "short": 0} for n, _ in variants}
for f, gt in items:
    raw = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
    if raw is None: continue
    for n, fn in variants:
        t = infer(fn(raw))
        res[n]["full"] += (t == gt)
        res[n]["prov"] += (len(t) > 0 and t[0] == gt[0])
        res[n]["short"] += (len(t) < len(gt))
N = len(items)
print("样本 %d 张 94x24 真实模型输入块" % N)
print("%-14s %-12s %-12s %-10s" % ("处理", "整串全对", "省字对", "少字"))
for n, _ in variants:
    print("%-14s %3d (%3.0f%%)   %3d (%3.0f%%)   %3d" % (n, res[n]["full"], 100.0*res[n]["full"]/N,
                                                          res[n]["prov"], 100.0*res[n]["prov"]/N, res[n]["short"]))
