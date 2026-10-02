# -*- coding: utf-8 -*-
"""验证"整数 RGB 版饱和度增强"能不能替代 HSV 版 —— 单片机上是逐像素 LUT/整数运算, 不用开方不用除法。
   原理: v = max(b,g,r); out_c = v + k*(c - v)  —— 保住最亮通道(值不变), 把色差拉开 = 提饱和, 色相不变。"""
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
D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images\京Q06666_难"
TRUTH = "京Q06666"
files = sorted(glob.glob(os.path.join(D, "*.jpg")))

def rgb_sat(im, k):
    f = im.astype(np.float32)
    v = f.max(axis=2, keepdims=True)
    out = np.clip(v + k * (f - v), 0, 255)
    return out.astype(np.uint8)

def new_box(img):
    b = img[:, :, 0].astype(np.int16); r = img[:, :, 2].astype(np.int16)
    m = (((b - r) > 40) & (b > 120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    n, lab, st, cen = cv2.connectedComponentsWithStats(m, 8)
    H, W = img.shape[:2]; best = None
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < 0.015*W*H or h < 12 or w < 30: continue
        if not (2.2 <= w/float(h) <= 4.2): continue
        if best is None or a > best[0]: best = (a, x, y, x+w, y+h)
    return None if best is None else best[1:]

def infer(img):
    im = cv2.resize(img, (94, 24)).astype(np.float32)
    return dec(net(torch.from_numpy(((im-127.5)*0.0078125).transpose(2,0,1)[None])).numpy()[0])

def crop_box(img, box, margin=0.03):
    H, W = img.shape[:2]; x0, y0, x1, y1 = box
    mx = (x1-x0)*margin; my = (y1-y0)*margin
    x0 = int(max(0,x0-mx)); y0 = int(max(0,y0-my)); x1 = int(min(W,x1+mx)); y1 = int(min(H,y1+my))
    return None if (x1-x0 < 8 or y1-y0 < 6) else img[y0:y1, x0:x1]

CASES = [("整数RGB k=1.7", lambda im: rgb_sat(im, 1.7)),
         ("整数RGB k=1.4", lambda im: rgb_sat(im, 1.4)),
         ("整数RGB k=2.0", lambda im: rgb_sat(im, 2.0)),
         ("HSV 饱和x1.7",  lambda im: T.preprocess(im, "sat"))]
print("%-16s %-8s %-10s %-10s" % ("处理", "找到框", "整串全对", "省字对"))
for name, fn in CASES:
    nb = nf = np_ = 0
    for f in files:
        raw = T.imread_u(f); e = fn(raw)
        bx = new_box(e)
        if bx is None: continue
        nb += 1
        cb = crop_box(e, bx)
        if cb is None: continue
        t = infer(cb)
        nf += (t == TRUTH); np_ += (len(t) > 0 and t[0] == TRUTH[0])
    print("%-16s %2d/20    %2d/20      %2d/20" % (name, nb, nf, np_))
# 顺带看看: 饱和增强后, 车牌区的 b-r 到底涨了多少
f0 = files[0]; raw = T.imread_u(f0); e = rgb_sat(raw, 1.7)
bx = new_box(e); x0,y0,x1,y1 = bx
for tag, im in (("原图", raw), ("增强", e)):
    p = im[y0:y1, x0:x1].astype(np.int16)
    print("%s 框内 b-r 中位 %d, 最大 %d" % (tag, np.median(p[:,:,0]-p[:,:,2]), (p[:,:,0]-p[:,:,2]).max()))
