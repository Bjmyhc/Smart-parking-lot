# -*- coding: utf-8 -*-
"""p565c: 汇总真实省字样本 (三个来源) -> out/p565/real_all/<省>/<n>.png, 32x64."""
import os, sys, glob, shutil
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
sys.path.insert(0, os.path.join(ROOT, "tools"))
import locator_replay as R
OUT = os.path.join(ROOT, "tools", "out", "p565", "real_all")
for lab in ("jing", "yue", "yu"):
    os.makedirs(os.path.join(OUT, lab), exist_ok=True)
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
def save(lab, im, n):
    cv2.imwrite(os.path.join(OUT, lab, "%04d.png" % n), im)
def frac_crop(plate, W=32, H=64):
    h, w = plate.shape[:2]
    x0 = int(round(w*0.030)); x1 = int(round(w*0.160))
    y0 = int(round(h*0.10));  y1 = int(round(h*0.90))
    return cv2.resize(plate[y0:y1, x0:x1], (W, H), interpolation=cv2.INTER_AREA)
def quad_eval(qx, qy, u, v):
    w0=(1-u)*(1-v); w1=u*(1-v); w2=u*v; w3=(1-u)*v
    return (w0*qx[0]+w1*qx[1]+w2*qx[2]+w3*qx[3], w0*qy[0]+w1*qy[1]+w2*qy[2]+w3*qy[3])
def quad_patch(im, qx, qy, frac=1.0/7.35, W=32, H=64):
    xs=(np.arange(W)[None,:]+0.5)*(frac/W); ys=(np.arange(H)[:,None]+0.5)/H
    U=np.broadcast_to(xs,(H,W)); V=np.broadcast_to(ys,(H,W))
    w0=(1-U)*(1-V); w1=U*(1-V); w2=U*V; w3=(1-U)*V
    mx=(w0*qx[0]+w1*qx[1]+w2*qx[2]+w3*qx[3]).astype(np.float32)
    my=(w0*qy[0]+w1*qy[1]+w2*qy[2]+w3*qy[3]).astype(np.float32)
    return cv2.remap(im, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
n = {"jing":0, "yue":0, "yu":0}
# 1) collect2 车牌块
c2 = os.path.join(ROOT, "tools", "out", "collect2")
for lab, sub in [("jing","京Q06666"), ("yue","粤T666FP")]:
    for p in sorted(glob.glob(os.path.join(c2, sub, "*.jpg"))):
        im = load(p)
        if im is not None: save(lab, frac_crop(im), n[lab]); n[lab] += 1
# 2) Doc 样例
d = r"G:\All_Project\AI_Project\Smart-Paring-Iot\Doc\硬件资料\车牌测试样例"
for lab, f in [("jing","京Q06666.png"), ("yue","粤T666FP.jpg"), ("yu","豫A8F8Q8.jpg"), ("yu","豫FSQ8I8.jpg")]:
    im = load(os.path.join(d, f))
    if im is not None: save(lab, frac_crop(im), n[lab]); n[lab] += 1
# 3) 难 素材 (定位 + 四边形采样)
class A: pass
a=A(); a.grid_step=None; a.s_blue=R.S_BLUE_DEF; a.s_green=R.S_GREEN_DEF; a.h_lo=R.H_LO; a.h_hi=R.H_HI
a.orig_w=320; a.orig_h=240; a.min_area_ratio=R.MIN_AREA_RATIO; a.min_fill=R.MIN_FILL
a.ratio_lo=R.RATIO_LO; a.ratio_hi=R.RATIO_HI; a.fit_min_fill=R.FIT_MIN_FILL
a.max_area_pct=0.0; a.no_fallback=False; a.no_edge_skip=False; a.no_image=True
a.keep_green_box=False; a.path=""; a.out_dir=None; a.csv=None; a.sweep_s=None; a.v_min=120.0
R.V_MIN_CUR=120.0
B = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
for folder, lab in [("京Q06666_难","jing"), ("豫FSQ818_难","yu"), ("豫A8F8Q8_难","yu")]:
    for p in sorted(glob.glob(os.path.join(B, folder, "*"))):
        im = load(p)
        if im is None or im.shape[0] != 240: continue
        work, mask, info = R.locate(im, a)
        ch = info.get("chosen") or {}
        fit = ch.get("fit") or {}
        if not fit.get("qx"): continue
        ws = info.get("work_size"); src = info.get("src_size")
        sx = float(src[0])/ws[0]; sy = float(src[1])/ws[1]
        qx=[float(v)*sx for v in fit["qx"]]; qy=[float(v)*sy for v in fit["qy"]]
        save(lab, quad_patch(im, qx, qy), n[lab]); n[lab] += 1
print("真实样本:", n, "合计", sum(n.values()))
# 拼图确认
rows=[]
for lab in ("jing","yue","yu"):
    fs = sorted(glob.glob(os.path.join(OUT, lab, "*.png")))[:4]
    for p in fs:
        im = load(p)
        rows.append(cv2.resize(im, (32*4, 64*4), interpolation=cv2.INTER_NEAREST))
cv2.imwrite(os.path.join(ROOT,"tools","out","p565","real_all_check.png"), np.vstack(rows))
