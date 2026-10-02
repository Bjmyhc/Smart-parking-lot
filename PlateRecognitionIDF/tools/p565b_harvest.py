# -*- coding: utf-8 -*-
"""p565b: 从"难"素材(320x240)抠高分辨率省字 -> 真实样本 (32x64)."""
import os, sys, glob, json
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
sys.path.insert(0, os.path.join(ROOT, "tools"))
import locator_replay as R
B = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
OUT = os.path.join(ROOT, "tools", "out", "p565", "real2"); os.makedirs(OUT, exist_ok=True)
class A: pass
a = A()
a.grid_step=None; a.s_blue=R.S_BLUE_DEF; a.s_green=R.S_GREEN_DEF; a.h_lo=R.H_LO; a.h_hi=R.H_HI
a.orig_w=320; a.orig_h=240; a.min_area_ratio=R.MIN_AREA_RATIO; a.min_fill=R.MIN_FILL
a.ratio_lo=R.RATIO_LO; a.ratio_hi=R.RATIO_HI; a.fit_min_fill=R.FIT_MIN_FILL
a.max_area_pct=0.0; a.no_fallback=False; a.no_edge_skip=False; a.no_image=True
a.keep_green_box=False; a.path=""; a.out_dir=None; a.csv=None; a.sweep_s=None; a.v_min=120.0
R.V_MIN_CUR = 120.0
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def quad_eval(qx, qy, u, v):
    w0=(1-u)*(1-v); w1=u*(1-v); w2=u*v; w3=(1-u)*v
    return (w0*qx[0]+w1*qx[1]+w2*qx[2]+w3*qx[3], w0*qy[0]+w1*qy[1]+w2*qy[2]+w3*qy[3])
def head_patch(im, qx, qy, frac=1.0/7.35, W=32, H=64):
    xs=(np.arange(W)[None,:]+0.5)*(frac/W); ys=(np.arange(H)[:,None]+0.5)/H
    U=np.broadcast_to(xs,(H,W)); V=np.broadcast_to(ys,(H,W))
    w0=(1-U)*(1-V); w1=U*(1-V); w2=U*V; w3=(1-U)*V
    mx=(w0*qx[0]+w1*qx[1]+w2*qx[2]+w3*qx[3]).astype(np.float32)
    my=(w0*qy[0]+w1*qy[1]+w2*qy[2]+w3*qy[3]).astype(np.float32)
    return cv2.remap(im, mx, my, cv2.INTER_AREA if False else cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
sets = [("京Q06666_难","jing"), ("豫FSQ818_难","yu"), ("豫A8F8Q8_难","yu")]
cnt = {}; samples = []
for folder, lab in sets:
    fs = sorted(glob.glob(os.path.join(B, folder, "*")))
    ok = 0
    for p in fs:
        im = load(p)
        if im is None or im.shape[0] != 240: continue
        work, mask, info = R.locate(im, a)
        ch = info.get("chosen")
        if not ch: continue
        fit = ch.get("fit") or {}
        if not fit.get("qx"): continue
        ws = info.get("work_size"); src = info.get("src_size")
        sx = float(src[0])/ws[0]; sy = float(src[1])/ws[1]
        qx = [float(v)*sx for v in fit["qx"]]; qy = [float(v)*sy for v in fit["qy"]]
        patch = head_patch(im, qx, qy)
        cv2.imwrite(os.path.join(OUT, "%s_%03d.png" % (lab, ok)), patch)
        ok += 1
        if len(samples) < 6: samples.append(cv2.resize(patch, (32*4, 64*4), interpolation=cv2.INTER_NEAREST))
    cnt[folder] = (ok, len(fs))
print(json.dumps(cnt, ensure_ascii=False))
if samples: cv2.imwrite(os.path.join(ROOT,"tools","out","p565","head_real2_check.png"), np.vstack(samples))
n_j = len(glob.glob(os.path.join(OUT, "jing_*.png"))); n_y = len(glob.glob(os.path.join(OUT, "yu_*.png")))
print("real2: jing=%d yu=%d" % (n_j, n_y))

