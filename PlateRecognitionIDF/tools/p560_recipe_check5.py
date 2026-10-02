# -*- coding: utf-8 -*-
"""p560_recipe_check5.py —— 收尾: 在 94x24 这一级扫 k; 并统计固件归一化实际用到的 s。"""
import os, sys, glob
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
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
def norm_s(im, target=200.0, s_max=3.0, min_spread=12):
    f = im.astype(np.float32)
    l = ((77.0*f[:,:,2] + 150.0*f[:,:,1] + 29.0*f[:,:,0]) / 256.0).astype(np.int32)
    hist = np.bincount(np.clip(l,0,255).ravel(), minlength=256); NP = l.size
    n_lo, n_hi = (NP*5)//100, (NP*95)//100
    cum=0; p_lo,p_hi=0,255; gl=gh=False
    for i in range(256):
        cum += int(hist[i])
        if not gl and cum>=n_lo: p_lo=i; gl=True
        if not gh and cum>=n_hi: p_hi=i; gh=True; break
    sp = p_hi-p_lo
    if sp < min_spread: return 1.0, sp
    return min(s_max, max(1.0, target/float(sp))), sp
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
        frames.append((truth, f, im))
N=len(frames)
print("样本 %d 帧; 定位固定 = 原图蓝块 -> 原图裁 -> 缩到 94x24" % N)
print()
print("-- 在 94x24 这一级做整数饱和度增强, 扫 k --")
print("%-10s %-16s %-16s" % ("k", "整串全对", "首字对"))
best=None
for k, k256 in ((1.0,256),(1.4,358),(1.7,435),(2.0,512),(2.3,589),(2.6,666),(3.0,768)):
    ex=pr=0
    for truth,f,im in frames:
        blk = crop_box(im, blue_box(im))
        if blk is None: continue
        blk = cv2.resize(blk, (94,24))
        if k256 != 256: blk = rgb_sat_int(blk, k256)
        t = infer94(blk)
        ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
    print("%-10s %2d/%d (%3.0f%%)    %2d/%d (%3.0f%%)" % (k, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
    if best is None or ex>best[1]: best=(k,ex)
print("  -> 最佳 k = %s" % best[0])
print()
print("-- 按车牌分开看 (k=2.0) --")
for folder, truth in SETS:
    ex=pr=nn=0
    for t2, f, im in frames:
        if t2 != truth: continue
        nn += 1
        blk = crop_box(im, blue_box(im)); blk = None if blk is None else cv2.resize(blk,(94,24))
        if blk is not None: blk = rgb_sat_int(blk, 512)
        t = infer94(blk)
        ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
    print("  %-14s 整串全对 %2d/%d (%3.0f%%)  首字对 %2d/%d (%3.0f%%)" % (folder, ex, nn, 100.0*ex/max(1,nn), pr, nn, 100.0*pr/max(1,nn)))
print()
print("-- 固件归一化在这批裁剪上实际用了多大的 s --")
sv=[]
for truth,f,im in frames:
    blk = crop_box(im, blue_box(im))
    if blk is None: continue
    blk = cv2.resize(blk,(94,24))
    s,sp = norm_s(blk); sv.append((s,sp))
sv=np.array(sv)
print("  s: 中位 %.2f  最大 %.2f | 用到 3.0 顶格的帧 %d/%d | 完全不动的帧(s=1) %d/%d" % (
    np.median(sv[:,0]), sv[:,0].max(), int((sv[:,0]>=2.999).sum()), len(sv), int((sv[:,0]<=1.001).sum()), len(sv)))
print("  亮度跨度 spread: 中位 %.0f  最小 %.0f  最大 %.0f (目标 200)" % (np.median(sv[:,1]), sv[:,1].min(), sv[:,1].max()))
