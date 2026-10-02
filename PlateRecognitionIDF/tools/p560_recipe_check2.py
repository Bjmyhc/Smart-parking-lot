# -*- coding: utf-8 -*-
"""p560_recipe_check2.py —— 受控对照: 框统一用"原图蓝块", 只换裁剪源; 并导出叠加图目视检查。"""
import os, sys, glob, argparse
import numpy as np, cv2, torch

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
SETS = [("京Q06666_难", "京Q06666"), ("豫FSQ818_难", "豫FSQ818"), ("豫A8F8Q8_难", "豫A8F8Q8")]
OUT = os.path.join(ROOT, "tools", "out", "p560")
os.makedirs(OUT, exist_ok=True)

blank = len(CHARS) - 1
def dec(out):
    a = np.squeeze(np.asarray(out, dtype=np.float32))
    if a.shape[0] != len(CHARS):
        a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank:
            res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res)

torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth"),
                               map_location="cpu"))
net.eval()
def infer(crop):
    if crop is None or crop.size == 0 or crop.shape[0] < 4 or crop.shape[1] < 8:
        return ""
    im = cv2.resize(crop, (94, 24)).astype(np.float32)
    im = (im - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2, 0, 1)[None])).numpy()[0])

def rgb_sat_int(img, k256=435):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)

def clahe_sat(img):
    lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB); l, a, b = cv2.split(lab)
    l = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(l)
    img = cv2.cvtColor(cv2.merge([l, a, b]), cv2.COLOR_LAB2BGR)
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV).astype(np.float32)
    hsv[:, :, 1] = np.clip(hsv[:, :, 1] * 1.7, 0, 255)
    return cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)

def blue_box(img, min_area_pct=1.5, ratio_lo=2.2, ratio_hi=4.2):
    H, W = img.shape[:2]
    b = img[:, :, 0].astype(np.int16); r = img[:, :, 2].astype(np.int16)
    m = (((b - r) > 40) & (b > 120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    n, lab, st, cen = cv2.connectedComponentsWithStats(m, 8)
    best = None
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < min_area_pct / 100.0 * W * H or h < 0.05 * H or w < 0.09 * W:
            continue
        if not (ratio_lo <= w / float(h) <= ratio_hi):
            continue
        if best is None or a > best[0]:
            best = (a, float(x), float(y), float(x + w), float(y + h))
    return None if best is None else best[1:]

def crop_box(img, box, margin=0.03):
    if box is None:
        return None
    H, W = img.shape[:2]; x0, y0, x1, y1 = box
    mx = (x1 - x0) * margin; my = (y1 - y0) * margin
    x0 = int(max(0, x0 - mx)); y0 = int(max(0, y0 - my))
    x1 = int(min(W, x1 + mx)); y1 = int(min(H, y1 + my))
    return None if (x1 - x0 < 8 or y1 - y0 < 6) else img[y0:y1, x0:x1]

def lev(a, b):
    m, n = len(a), len(b); d = list(range(n + 1))
    for i in range(1, m + 1):
        prev = d[0]; d[0] = i
        for j in range(1, n + 1):
            cur = d[j]; d[j] = min(d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] != b[j - 1])); prev = cur
    return d[n]

CROPS = [("原图裁", lambda im: im),
         ("整1.7增强裁", lambda im: rgb_sat_int(im, 435)),
         ("整1.4增强裁", lambda im: rgb_sat_int(im, 358)),
         ("整2.0增强裁", lambda im: rgb_sat_int(im, 512)),
         ("CLAHE+饱和裁", clahe_sat)]

print("=" * 100)
print("受控对照: 框统一 = 原图蓝块(不增强); 只换裁剪源")
print("=" * 100)
grand = {}
for folder, truth in SETS:
    files = sorted(glob.glob(os.path.join(BASE, folder, "*.jpg")))
    res = {n: {"full": 0, "prov": 0, "ed": 0, "n": 0} for n, _ in CROPS}
    shown = 0
    for f in files:
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240:
            continue
        box = blue_box(im)
        e = rgb_sat_int(im, 435)
        for n, fn in CROPS:
            src = fn(im)
            t = infer(crop_box(src, box))
            r = res[n]; r["n"] += 1
            r["full"] += (t == truth); r["prov"] += (len(t) > 0 and t[0] == truth[0])
            r["ed"] += lev(t, truth)
        if shown < 6 and box is not None:
            vis = im.copy()
            cv2.rectangle(vis, (int(box[0]), int(box[1])), (int(box[2]), int(box[3])), (0, 255, 0), 2)
            cv2.imwrite(os.path.join(OUT, "%s_%02d_box.png" % (folder, shown)), vis)
            shown += 1
    n = max(1, res[CROPS[0][0]]["n"])
    print()
    print("---- %s (%d 帧) ----" % (folder, n))
    print("%-16s %-16s %-14s %-10s" % ("裁剪源", "整串全对", "首字对", "平均编辑距离"))
    for name, _ in CROPS:
        r = res[name]
        print("%-16s %2d/%d (%3.0f%%)   %2d/%d (%3.0f%%)   %.2f" % (
            name, r["full"], n, 100.0 * r["full"] / n, r["prov"], n, 100.0 * r["prov"] / n, r["ed"] / float(n)))
    for name, _ in CROPS:
        g = grand.setdefault(name, {"full": 0, "n": 0, "prov": 0})
        g["full"] += res[name]["full"]; g["n"] += res[name]["n"]; g["prov"] += res[name]["prov"]

print()
print("=" * 100)
print("三组合计 (n=%d)" % grand[CROPS[0][0]]["n"])
for name, _ in CROPS:
    g = grand[name]; n = max(1, g["n"])
    print("%-16s 整串全对 %2d/%d (%3.0f%%)   首字对 %2d/%d (%3.0f%%)" % (
        name, g["full"], n, 100.0 * g["full"] / n, g["prov"], n, 100.0 * g["prov"] / n))
print()
print("叠加图 ->", OUT)

