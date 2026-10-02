# -*- coding: utf-8 -*-
"""p560_mask_swap2.py —— 换判据后, 扫"面积下限/填充/比例/贴边", 找能追平 67% 的最小改动组合。"""
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
def rgb_sat_int(img, k256=512):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)
def crop_box(img, box, margin=0.03):
    if box is None: return None
    H,W = img.shape[:2]; x0,y0,x1,y1 = box
    mx=(x1-x0)*margin; my=(y1-y0)*margin
    x0=int(max(0,x0-mx)); y0=int(max(0,y0-my)); x1=int(min(W,x1+mx)); y1=int(min(H,y1+my))
    return None if (x1-x0<8 or y1-y0<6) else img[y0:y1, x0:x1]
ORIG_MASK = R.plate_mask
def mask_br(bgr, s_blue, s_green, h_lo, h_hi):
    b = bgr[:,:,0].astype(np.int32); r = bgr[:,:,2].astype(np.int32)
    return ((b - r) > 40) & (b > 120)
frames=[]
for folder,truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0]!=240: continue
        frames.append((folder, truth, im))
N=len(frames)
R.V_MIN_CUR = 120
R.plate_mask = mask_br
R.S_BLUE_MIN = 35
print("换判据(b-r>40,b>120)后, 扫其余门槛:  (n=%d)" % N)
print("%-52s %-12s %-14s %-14s" % ("配置", "找到框", "整串全对", "首字对"))
CASES = []
for area in (0.002, 0.015, 0.020, 0.030):
    for fill in (0.60, 0.40, 0.20):
        CASES.append((area, fill, 2.2, 4.2, False))
CASES += [(0.020, 0.20, 2.0, 6.5, False), (0.020, 0.20, 2.2, 4.2, True),
          (0.015, 0.20, 2.2, 4.2, True), (0.030, 0.20, 2.2, 4.2, True)]
for area, fill, rl, rh, noedge in CASES:
    A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                           min_area_ratio=area, ratio_lo=rl, ratio_hi=rh, min_fill=fill,
                           fit_min_fill=0.35, max_area_pct=0.0, no_fallback=False,
                           no_edge_skip=noedge, keep_green_box=False, orig_w=320, orig_h=240)
    nb=ex=pr=0
    for folder,truth,im in frames:
        A.orig_w, A.orig_h = im.shape[1], im.shape[0]
        work, mask, info = R.locate(im.copy(), A)
        ch = info.get("chosen"); box=None
        if ch:
            bx = ch["box"]
            sy = im.shape[0]/float(work.shape[0]); sx = im.shape[1]/float(work.shape[1])
            box=(bx["x1"]*sx, bx["y1"]*sy, bx["x2"]*sx, bx["y2"]*sy); nb+=1
        blk = crop_box(im, box)
        blk = None if blk is None else rgb_sat_int(cv2.resize(blk,(94,24)), 512)
        t = infer94(blk)
        ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
    print("面积下限%.3f 填充%.2f 比例%.1f~%.1f 贴边%s            %2d/%d      %2d/%d (%3.0f%%)  %2d/%d (%3.0f%%)" % (
        area, fill, rl, rh, "照用" if noedge else "拒绝", nb, N, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
R.plate_mask = ORIG_MASK
