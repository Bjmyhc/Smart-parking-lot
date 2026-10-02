# -*- coding: utf-8 -*-
"""p560_final_cfg.py —— 定稿前最后确认: 与将要改的固件常量逐一对齐的复算。"""
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
def crop_box(img, box, ml=0.01, mr=0.08, mv=0.08):
    """按固件默认档2的留白: 左 1% 右 8% 上下各 8%"""
    if box is None: return None
    H,W = img.shape[:2]; x0,y0,x1,y1 = box
    x0=int(max(0,x0-(x1-x0)*ml)); x1=int(min(W,x1+(x1-x0)*mr))
    y0=int(max(0,y0-(y1-y0)*mv)); y1=int(min(H,y1+(y1-y0)*mv))
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
def run(maskfn, ratio_lo, ratio_hi, noedge, sat, tag):
    R.plate_mask = maskfn; R.S_BLUE_MIN = 35
    A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                           min_area_ratio=0.02, ratio_lo=ratio_lo, ratio_hi=ratio_hi, min_fill=0.60,
                           fit_min_fill=0.35, max_area_pct=45.0, no_fallback=False,
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
        # locate() 内部已按 TRIM 收过 1%/3%, 这里不再另加外边距
        blk = crop_box(im, box)
        if blk is not None:
            blk = cv2.resize(blk,(94,24))
            if sat: blk = rgb_sat_int(blk, 512)
        else:
            blk = None
        t = infer94(blk)
        ex += (t==truth); pr += (len(t)>0 and t[0]==truth[0])
    R.plate_mask = ORIG_MASK
    print("%-46s 找到框 %2d/%d | 整串全对 %2d/%d (%3.0f%%) | 首字对 %2d/%d (%3.0f%%)" % (
        tag, nb, N, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
run(ORIG_MASK, 2.0, 6.5, False, True, "改前(现状): HSV掩码 比例2.0~6.5 贴边拒 剪裁+饱和")
run(mask_br,   2.2, 4.2, False, True, "改后: 判据b-r 比例2.2~4.2 贴边拒 剪裁+饱和")
run(mask_br,   2.2, 4.2, True,  True, "改后: 判据b-r 比例2.2~4.2 贴边照用 剪裁+饱和")
run(mask_br,   2.2, 4.2, True,  False, "改后(去掉剪裁后饱和, 作对照)")
