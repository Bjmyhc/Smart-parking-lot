# -*- coding: utf-8 -*-
"""p560_recipe_check.py —— 用新拍的原始帧复核 P5.60 配方:
    全帧整数 RGB 饱和度增强 -> 在增强图上找蓝块当框 -> 裁剪也从增强图取 -> 接 r4 模型。
对照 5 个变体, 看配方在"换车牌 / 换距离 / 带偏角"上还成不成立。

用法:  python p560_recipe_check.py
"""
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

# ---------------- 模型 ----------------
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
    x = torch.from_numpy(im.transpose(2, 0, 1)[None])
    return dec(net(x).numpy()[0])

# ---------------- 增强 ----------------
def rgb_sat_int(img, k256=435):
    """固件要用的整数版: v=max(b,g,r); out_c = v + ((c-v)*k256 >> 8)"""
    f = img.astype(np.int32)
    v = f.max(axis=2, keepdims=True)
    d = f - v
    return np.clip(v + ((d * k256) // 256), 0, 255).astype(np.uint8)

def rgb_sat_float(img, k=1.7):
    f = img.astype(np.float32)
    v = f.max(axis=2, keepdims=True)
    return np.clip(v + k * (f - v), 0, 255).astype(np.uint8)

def clahe_sat(img):
    lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB)
    l, a, b = cv2.split(lab)
    l = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(l)
    img = cv2.cvtColor(cv2.merge([l, a, b]), cv2.COLOR_LAB2BGR)
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV).astype(np.float32)
    hsv[:, :, 1] = np.clip(hsv[:, :, 1] * 1.7, 0, 255)
    return cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)

# ---------------- 蓝块定位 ----------------
def blue_box(img, min_area_pct=1.5, ratio_lo=2.2, ratio_hi=4.2):
    H, W = img.shape[:2]
    b = img[:, :, 0].astype(np.int16); r = img[:, :, 2].astype(np.int16)
    m = (((b - r) > 40) & (b > 120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    n, lab, st, cen = cv2.connectedComponentsWithStats(m, 8)
    best = None
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < min_area_pct / 100.0 * W * H:
            continue
        if h < 0.05 * H or w < 0.09 * W:
            continue
        ratio = w / float(h)
        if not (ratio_lo <= ratio <= ratio_hi):
            continue
        if best is None or a > best[0]:
            best = (a, float(x), float(y), float(x + w), float(y + h))
    return None if best is None else best[1:]

def crop_box(img, box, margin=0.03):
    if box is None:
        return None
    H, W = img.shape[:2]
    x0, y0, x1, y1 = box
    mx = (x1 - x0) * margin; my = (y1 - y0) * margin
    x0 = int(max(0, x0 - mx)); y0 = int(max(0, y0 - my))
    x1 = int(min(W, x1 + mx)); y1 = int(min(H, y1 + my))
    if x1 - x0 < 8 or y1 - y0 < 6:
        return None
    return img[y0:y1, x0:x1]

# ---------------- 固件现状定位 (locator_replay) ----------------
A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                       min_area_ratio=0.002, ratio_lo=2.0, ratio_hi=6.5, min_fill=0.60,
                       fit_min_fill=0.35, max_area_pct=0.0, no_fallback=False,
                       no_edge_skip=False, keep_green_box=False, orig_w=320, orig_h=240)

def replay_box(img):
    A.orig_w, A.orig_h = img.shape[1], img.shape[0]
    work, mask, info = R.locate(img.copy(), A)
    ch = info.get("chosen")
    if not ch:
        return None, info.get("verdict")
    bx = ch["box"]
    # locate() 内部可能把图 resize 到网格尺寸, 这里换算回原图坐标
    sy = img.shape[0] / float(work.shape[0]); sx = img.shape[1] / float(work.shape[1])
    return (bx["x1"] * sx, bx["y1"] * sy, bx["x2"] * sx, bx["y2"] * sy), info.get("verdict")

def lev(a, b):
    m, n = len(a), len(b)
    d = list(range(n + 1))
    for i in range(1, m + 1):
        prev = d[0]; d[0] = i
        for j in range(1, n + 1):
            cur = d[j]
            d[j] = min(d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] != b[j - 1]))
            prev = cur
    return d[n]

# ---------------- 对照变体 ----------------
VARIANTS = [
    ("A 固件现状(replay)",   lambda im, e: (replay_box(im)[0], im)),
    ("B 原图蓝块@原图裁",     lambda im, e: (blue_box(im), im)),
    ("C 增强蓝块@原图裁",     lambda im, e: (blue_box(e), im)),
    ("D 增强蓝块@增强裁(配方)", lambda im, e: (blue_box(e), e)),
    ("E CLAHE+饱和 同上",     lambda im, e: (blue_box(clahe_sat(im)), clahe_sat(im))),
    ("F 整帧直接缩(不定位)",   lambda im, e: ((0.0, 0.0, float(im.shape[1]), float(im.shape[0])), im)),
]

def run(tag_filter=None):
    print("=" * 108)
    print("配方复核: 新拍原始帧 (320x240 预览图)")
    print("=" * 108)
    grand = {}
    for folder, truth in SETS:
        files = sorted(glob.glob(os.path.join(BASE, folder, "*.jpg")))
        if not files:
            continue
        res = {v[0]: {"box": 0, "full": 0, "prov": 0, "ed": 0, "n": 0} for v in VARIANTS}
        print()
        print("---- %s  (%d 帧, 真值 %s) ----" % (folder, len(files), truth))
        for f in files:
            im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
            if im is None or im.shape[0] != 240:
                continue
            e = rgb_sat_int(im)
            for name, fn in VARIANTS:
                box, src = fn(im, e)
                r = res[name]
                r["n"] += 1
                if box is not None:
                    r["box"] += 1
                t = infer(crop_box(src, box))
                r["full"] += (t == truth)
                r["prov"] += (len(t) > 0 and t[0] == truth[0])
                r["ed"] += lev(t, truth)
        n = max(1, res[VARIANTS[0][0]]["n"])
        print("%-26s %-12s %-12s %-12s %-10s" % ("变体", "找到框", "整串全对", "首字对", "平均编辑距离"))
        for name, _ in VARIANTS:
            r = res[name]
            print("%-26s %2d/%d (%3.0f%%)  %2d/%d (%3.0f%%)  %2d/%d (%3.0f%%)  %.2f" % (
                name, r["box"], n, 100.0 * r["box"] / n,
                r["full"], n, 100.0 * r["full"] / n,
                r["prov"], n, 100.0 * r["prov"] / n, r["ed"] / float(n)))
        for name, _ in VARIANTS:
            g = grand.setdefault(name, {"full": 0, "n": 0, "prov": 0})
            g["full"] += res[name]["full"]; g["n"] += res[name]["n"]; g["prov"] += res[name]["prov"]
    print()
    print("=" * 108)
    print("三组合计  (n=%d)" % grand[VARIANTS[0][0]]["n"])
    print("%-26s %-16s %-16s" % ("变体", "整串全对", "首字对"))
    for name, _ in VARIANTS:
        g = grand[name]
        n = max(1, g["n"])
        print("%-26s %2d/%d (%3.0f%%)    %2d/%d (%3.0f%%)" % (
            name, g["full"], n, 100.0 * g["full"] / n, g["prov"], n, 100.0 * g["prov"] / n))

if __name__ == "__main__":
    run()
