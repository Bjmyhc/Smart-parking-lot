# -*- coding: utf-8 -*-
"""p560_recipe_check3.py —— 定旋钮: k 值 / 裁剪边距; 并看错误分布、导出失败帧。"""
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
def infer(crop):
    if crop is None or crop.size == 0 or crop.shape[0] < 4 or crop.shape[1] < 8: return ""
    im = cv2.resize(crop, (94, 24)).astype(np.float32)
    im = (im - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2, 0, 1)[None])).numpy()[0])
def rgb_sat_int(img, k256):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)
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
def crop_box(img, box, margin):
    if box is None: return None
    H, W = img.shape[:2]; x0, y0, x1, y1 = box
    mx = (x1 - x0) * margin; my = (y1 - y0) * margin
    x0 = int(max(0, x0 - mx)); y0 = int(max(0, y0 - my))
    x1 = int(min(W, x1 + mx)); y1 = int(min(H, y1 + my))
    return None if (x1 - x0 < 8 or y1 - y0 < 6) else img[y0:y1, x0:x1]

frames = []
for folder, truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240: continue
        frames.append((folder, truth, f, im, blue_box(im)))
N = len(frames)
print("样本 %d 帧, 找到框 %d 帧" % (N, sum(1 for x in frames if x[4] is not None)))
print()
print("-- k 值扫描 (裁剪边距 0.03) --")
print("%-10s %-16s %-16s" % ("k", "整串全对", "首字对"))
best = None
for k, k256 in ((1.4, 358), (1.7, 435), (2.0, 512), (2.3, 589), (2.6, 666)):
    ex = pr = 0
    for folder, truth, f, im, box in frames:
        t = infer(crop_box(rgb_sat_int(im, k256), box, 0.03))
        ex += (t == truth); pr += (len(t) > 0 and t[0] == truth[0])
    print("%-10s %2d/%d (%3.0f%%)    %2d/%d (%3.0f%%)" % (k, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
    if best is None or ex > best[1]: best = (k, ex, k256)
K = best[0]; K256 = best[2]
print("  -> 最佳 k = %s" % K)
print()
print("-- 裁剪边距扫描 (k=%s) --" % K)
print("%-10s %-16s %-16s" % ("边距", "整串全对", "首字对"))
bm = None
for mg in (0.0, 0.03, 0.08, 0.15):
    ex = pr = 0
    for folder, truth, f, im, box in frames:
        t = infer(crop_box(rgb_sat_int(im, K256), box, mg))
        ex += (t == truth); pr += (len(t) > 0 and t[0] == truth[0])
    print("%-10s %2d/%d (%3.0f%%)    %2d/%d (%3.0f%%)" % (mg, ex, N, 100.0*ex/N, pr, N, 100.0*pr/N))
    if bm is None or ex > bm[1]: bm = (mg, ex)
MG = bm[0]
print("  -> 最佳边距 = %s" % MG)
print()
print("-- 逐位正确率 (k=%s, 边距=%s) --" % (K, MG))
pos_hit = [0]*8; pos_tot = [0]*8
fail = []
for folder, truth, f, im, box in frames:
    src = rgb_sat_int(im, K256)
    t = infer(crop_box(src, box, MG))
    for j in range(len(truth)):
        pos_tot[j] += 1
        if j < len(t) and t[j] == truth[j]: pos_hit[j] += 1
    if t != truth:
        fail.append((folder, os.path.basename(f), t, truth))
for j in range(len(pos_tot)):
    if pos_tot[j]:
        print("   第 %d 位: %2d/%d (%3.0f%%)" % (j+1, pos_hit[j], pos_tot[j], 100.0*pos_hit[j]/pos_tot[j]))
print()
print("失败 %d 帧:" % len(fail))
for folder, name, t, truth in fail[:45]:
    print("   %-14s %-32s 预测 %-12s 真值 %s" % (folder, name, t, truth))
# 拼一张失败帧的裁剪缩略图
tiles = []
for folder, truth, f, im, box in frames:
    src = rgb_sat_int(im, K256)
    c = crop_box(src, box, MG)
    t = infer(c)
    if t != truth and c is not None:
        tiles.append(cv2.resize(c, (188, 48)))
if tiles:
    rows = []
    for i in range(0, min(len(tiles), 40), 4):
        row = tiles[i:i+4]
        while len(row) < 4: row.append(np.zeros((48, 188, 3), np.uint8))
        rows.append(np.hstack(row))
    cv2.imwrite(os.path.join(OUT, "fails_montage.png"), np.vstack(rows))
    print()
    print("失败帧裁剪拼图 ->", os.path.join(OUT, "fails_montage.png"))
