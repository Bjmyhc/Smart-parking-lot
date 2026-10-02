# -*- coding: utf-8 -*-
"""p560_recipe_check4.py —— 固件裁剪里已有 crop_photometric_norm(对比度+饱和度自适应拉伸)。
本脚本把它在 PC 上按同一公式复刻, 回答: "新加的全帧饱和度增强" 与它是否重复/叠加。"""
import os, sys, glob
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
SETS = [("京Q06666_难", "京Q06666"), ("豫FSQ818_难", "豫FSQ818"), ("豫A8F8Q8_难", "豫A8F8Q8")]
OUT = os.path.join(ROOT, "tools", "out", "p560"); os.makedirs(OUT, exist_ok=True)
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
    if blk is None or blk.size == 0: return ''
    im = blk.astype(np.float32)
    im = (im - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2, 0, 1)[None])).numpy()[0])

def rgb_sat_int(img, k256=512):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)

def photo_norm(im, target=200.0, s_max=3.0, min_spread=12):
    f = im.astype(np.float32)
    l = ((77.0 * f[:, :, 2] + 150.0 * f[:, :, 1] + 29.0 * f[:, :, 0]) / 256.0).astype(np.int32)
    hist = np.bincount(np.clip(l, 0, 255).ravel(), minlength=256)
    NP = l.size
    n_lo, n_hi = (NP * 5) // 100, (NP * 95) // 100
    cum = 0; p_lo, p_hi = 0, 255; gl = gh = False
    for i in range(256):
        cum += int(hist[i])
        if not gl and cum >= n_lo: p_lo = i; gl = True
        if not gh and cum >= n_hi: p_hi = i; gh = True; break
    spread = p_hi - p_lo
    s = 1.0
    if spread >= min_spread:
        s = target / float(spread)
        s = min(s_max, max(1.0, s))
    if s <= 1.0:
        return im
    out = np.empty_like(f)
    for c in range(3):
        m = f[:, :, c].mean()
        v = m + (f[:, :, c] - m) * s + np.where(f[:, :, c] > m, 0.5, -0.5)
        out[:, :, c] = np.clip(v, 0, 255)
    return out.astype(np.uint8)

def blue_box(img, min_area_pct=1.5, ratio_lo=2.2, ratio_hi=4.2):
    H, W = img.shape[:2]
    b = img[:, :, 0].astype(np.int16); r = img[:, :, 2].astype(np.int16)
    m = (((b - r) > 40) & (b > 120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    n, lab, st, cen = cv2.connectedComponentsWithStats(m, 8)
    best = None
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < min_area_pct / 100.0 * W * H or h < 0.05 * H or w < 0.09 * W: continue
        if not (ratio_lo <= w / float(h) <= ratio_hi): continue
        if best is None or a > best[0]: best = (a, float(x), float(y), float(x + w), float(y + h))
    return None if best is None else best[1:]
def crop_box(img, box, margin=0.03):
    if box is None: return None
    H, W = img.shape[:2]; x0, y0, x1, y1 = box
    mx = (x1 - x0) * margin; my = (y1 - y0) * margin
    x0 = int(max(0, x0 - mx)); y0 = int(max(0, y0 - my))
    x1 = int(min(W, x1 + mx)); y1 = int(min(H, y1 + my))
    return None if (x1 - x0 < 8 or y1 - y0 < 6) else img[y0:y1, x0:x1]
def to94(c):
    return None if c is None else cv2.resize(c, (94, 24))

frames = []
for folder, truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240: continue
        frames.append((folder, truth, f, im))
N = len(frames)

CFG = [
    ("原图裁 + 无归一化",      lambda o, e: to94(crop_box(o, blue_box(o)))),
    ("原图裁 + 固件归一化",     lambda o, e: (lambda b: b if b is None else photo_norm(b))(to94(crop_box(o, blue_box(o))))),
    ("增强裁k2.0 + 无归一化",   lambda o, e: to94(crop_box(e, blue_box(o)))),
    ("增强裁k2.0 + 固件归一化",  lambda o, e: (lambda b: b if b is None else photo_norm(b))(to94(crop_box(e, blue_box(o))))),
    ("原图裁→缩放后增强 + 无归一化", lambda o, e: (lambda b: b if b is None else rgb_sat_int(b, 512))(to94(crop_box(o, blue_box(o))))),
]
print("样本 %d 帧" % N)
print("%-30s %-16s %-16s" % ("配置", "整串全对", "首字对"))
res = {}
for name, fn in CFG:
    ex = pr = 0
    for folder, truth, f, im in frames:
        e = rgb_sat_int(im, 512)
        t = infer94(fn(im, e))
        ex += (t == truth); pr += (len(t) > 0 and t[0] == truth[0])
    res[name] = (ex, pr)
    print("%-30s %2d/%d (%3.0f%%)    %2d/%d (%3.0f%%)" % (name, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))

# 目视: 同一块车牌, 三种处理的样子
sample = None
for folder, truth, f, im in frames:
    if folder.startswith("豫FSQ"):
        sample = im; break
if sample is not None:
    o = sample; e = rgb_sat_int(o, 512); bx = blue_box(o)
    c0 = to94(crop_box(o, bx)); c1 = photo_norm(c0); c2 = to94(crop_box(e, bx))
    sheet = np.hstack([cv2.resize(x, (282, 72), interpolation=cv2.INTER_NEAREST) for x in (c0, c1, c2)])
    cv2.imwrite(os.path.join(OUT, "norm_vs_enh.png"), sheet)
    print()
    print("三种处理的对比图(左: 原图裁 | 中: +固件归一化 | 右: 全帧增强后裁) ->", os.path.join(OUT, "norm_vs_enh.png"))

