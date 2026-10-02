# -*- coding: utf-8 -*-
"""p560_diag_box.py —— 诊断: 固件那套定位(改判据后)在 豫A8F8Q8_难 上给出的框, 到底比车牌大多少?
   同时算"框内蓝带"(沿长边的蓝色覆盖区间), 与设备日志的 蓝带[..] 对照。"""
import os, sys, glob, argparse
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
sys.path.insert(0, os.path.join(ROOT, "tools"))
import locator_replay as R
BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT  = os.path.join(ROOT, "tools", "out", "p560"); os.makedirs(OUT, exist_ok=True)

ORIG_MASK = R.plate_mask
def mask_br(bgr, s_blue, s_green, h_lo, h_hi):
    b = bgr[:,:,0].astype(np.int32); r = bgr[:,:,2].astype(np.int32)
    return ((b - r) > 40) & (b > 120)
R.plate_mask = mask_br
R.V_MIN_CUR = 120
R.S_BLUE_MIN = 35
A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                       min_area_ratio=0.02, ratio_lo=2.2, ratio_hi=4.2, min_fill=0.60,
                       fit_min_fill=0.35, max_area_pct=45.0, no_fallback=False,
                       no_edge_skip=True, keep_green_box=False, orig_w=320, orig_h=240)

def tight_box(img):
    H,W = img.shape[:2]
    b = img[:,:,0].astype(np.int16); r = img[:,:,2].astype(np.int16)
    m = (((b-r)>40) & (b>120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3,3), np.uint8))
    n,lab,st,cen = cv2.connectedComponentsWithStats(m, 8)
    best=None
    for i in range(1,n):
        x,y,w,h,a = st[i]
        if a < 0.015*W*H or h < 0.05*H or w < 0.09*W: continue
        if not (2.2 <= w/float(h) <= 4.2): continue
        if best is None or a > best[0]: best=(a,float(x),float(y),float(x+w),float(y+h))
    return None if best is None else best[1:]

def blue_band(img, box):
    """沿长边方向, 每列的"蓝像素占比>=25%"算蓝列; 返回第一个/最后一个蓝列的相对位置。"""
    x0,y0,x1,y1 = [int(round(v)) for v in box]
    x0=max(0,x0); y0=max(0,y0); x1=min(img.shape[1],x1); y1=min(img.shape[0],y1)
    p = img[y0:y1, x0:x1].astype(np.int16)
    if p.size == 0: return (None, None)
    b, r = p[:,:,0], p[:,:,2]
    m = ((b - r) > 40) & (b > 120)
    col = m.mean(axis=0)
    idx = np.where(col >= 0.25)[0]
    if len(idx) == 0: return (None, None)
    w = x1 - x0
    return (100.0*idx[0]/w, 100.0*(idx[-1]+1)/w)

files = sorted(glob.glob(os.path.join(BASE, "豫A8F8Q8_难", "*.jpg")))
print("帧                              固件框(320x240)    ->640x480     比例  框内蓝色%  蓝带%")
rows=[]
for i,f in enumerate(files):
    im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
    if im is None: continue
    A.orig_w, A.orig_h = im.shape[1], im.shape[0]
    work, mask, info = R.locate(im.copy(), A)
    ch = info.get("chosen"); box=None
    if ch:
        bx = ch["box"]
        sy = im.shape[0]/float(work.shape[0]); sx = im.shape[1]/float(work.shape[1])
        box=(bx["x1"]*sx, bx["y1"]*sy, bx["x2"]*sx, bx["y2"]*sy)
    tb = tight_box(im)
    name = os.path.basename(f)
    if box is None:
        print("%-30s  没找到" % name); continue
    bw = box[2]-box[0]; bh = box[3]-box[1]
    x0,y0,x1,y1 = [int(round(v)) for v in box]
    x0=max(0,x0); y0=max(0,y0); x1=min(im.shape[1],x1); y1=min(im.shape[0],y1)
    p = im[y0:y1, x0:x1].astype(np.int16)
    bf = float((((p[:,:,0]-p[:,:,2])>40) & (p[:,:,0]>120)).mean())*100 if p.size else 0
    lo, hi = blue_band(im, box)
    print("%-30s  %3.0fx%-3.0f          %3.0fx%-3.0f   %.2f   %5.1f%%    %s" % (
        name, bw, bh, bw*2, bh*2, bw/bh, bf,
        ("%.0f%%~%.0f%%" % (lo, hi)) if lo is not None else "n/a"))
    rows.append((f, box, tb, info.get("verdict")))
    if i < 6:
        vis = im.copy()
        cv2.rectangle(vis, (int(box[0]), int(box[1])), (int(box[2]), int(box[3])), (0,255,0), 2)
        if tb is not None:
            cv2.rectangle(vis, (int(tb[0]), int(tb[1])), (int(tb[2]), int(tb[3])), (0,0,255), 1)
        cv2.imwrite(os.path.join(OUT, "diag_%02d.png" % i), vis)
print()
print("绿 = 固件那套机器给出的框; 红 = 纯蓝块紧贴框 ->", OUT)
