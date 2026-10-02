# -*- coding: utf-8 -*-
"""p560_base_fix.py —— 修正基线: locator_replay 的亮度下限必须跟固件一样是 120 (ROI_MIN_VALUE)。"""
import os, sys, glob, argparse
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
import locator_replay as R
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
SETS = [("京Q06666_难", "京Q06666"), ("豫FSQ818_难", "豫FSQ818"), ("豫A8F8Q8_难", "豫A8F8Q8")]
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
def infer94(blk):
    if blk is None or blk.size == 0: return ""
    im = (blk.astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2, 0, 1)[None])).numpy()[0])
def rgb_sat_int(img, k256):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)
def blue_box(img, min_area_pct=1.5, ratio_lo=2.2, ratio_hi=4.2):
    H,W = img.shape[:2]
    b = img[:,:,0].astype(np.int16); r = img[:,:,2].astype(np.int16)
    m = (((b-r)>40) & (b>120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3,3), np.uint8))
    n,lab,st,cen = cv2.connectedComponentsWithStats(m, 8)
    best=None
    for i in range(1,n):
        x,y,w,h,a = st[i]
        if a < min_area_pct/100.0*W*H or h < 0.05*H or w < 0.09*W: continue
        if not (ratio_lo <= w/float(h) <= ratio_hi): continue
        if best is None or a > best[0]: best=(a,float(x),float(y),float(x+w),float(y+h))
    return None if best is None else best[1:]
def crop_box(img, box, margin=0.03):
    if box is None: return None
    H,W = img.shape[:2]; x0,y0,x1,y1 = box
    mx=(x1-x0)*margin; my=(y1-y0)*margin
    x0=int(max(0,x0-mx)); y0=int(max(0,y0-my)); x1=int(min(W,x1+mx)); y1=int(min(H,y1+my))
    return None if (x1-x0<8 or y1-y0<6) else img[y0:y1, x0:x1]

frames=[]
for folder,truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0]!=240: continue
        frames.append((folder, truth, im))
N=len(frames)

for vmin in (46, 120):
    R.V_MIN_CUR = vmin
    A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                           min_area_ratio=0.002, ratio_lo=2.0, ratio_hi=6.5, min_fill=0.60,
                           fit_min_fill=0.35, max_area_pct=0.0, no_fallback=False,
                           no_edge_skip=False, keep_green_box=False, orig_w=320, orig_h=240)
    R.S_BLUE_MIN = min(A.s_blue, A.s_green)
    nb=ex=pr=0
    for folder,truth,im in frames:
        A.orig_w, A.orig_h = im.shape[1], im.shape[0]
        work, mask, info = R.locate(im.copy(), A)
        ch = info.get("chosen")
        box=None
        if ch:
            bx = ch["box"]
            sy = im.shape[0]/float(work.shape[0]); sx = im.shape[1]/float(work.shape[1])
            box=(bx["x1"]*sx, bx["y1"]*sy, bx["x2"]*sx, bx["y2"]*sy)
            nb += 1
        blk = crop_box(im, box)
        blk = None if blk is None else cv2.resize(blk,(94,24))
        if blk is not None: blk = rgb_sat_int(blk, 512)
        t = infer94(blk)
        ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
    print("固件现状定位 (v-min=%3d): 找到框 %2d/%d | 整串全对 %2d/%d (%3.0f%%) | 首字对 %2d/%d (%3.0f%%)" % (
        vmin, nb, N, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))

nb=ex=pr=0
for folder,truth,im in frames:
    box = blue_box(im)
    if box is not None: nb += 1
    blk = crop_box(im, box)
    blk = None if blk is None else cv2.resize(blk,(94,24))
    if blk is not None: blk = rgb_sat_int(blk, 512)
    t = infer94(blk)
    ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
print("新定位 原图蓝块          : 找到框 %2d/%d | 整串全对 %2d/%d (%3.0f%%) | 首字对 %2d/%d (%3.0f%%)" % (
    nb, N, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
