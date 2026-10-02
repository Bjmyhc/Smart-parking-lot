# -*- coding: utf-8 -*-
"""e2e_box_test.py —— 端到端复算: 不同的"定位框"分别裁出来喂 r4 模型, 比识别对不对。

2026-10-01: 目的是回答"定位改好了, 识别到底涨不涨"。三种组合:
    现状框      = 固件现在的定位 (locator_replay 复算, 见 out/replay_base.csv)
    新框        = 增强图(CLAHE+饱和度x1.7)上的最大蓝块
    新框+增强图 = 同上, 但裁剪也从增强图上取(验证"把一切都归一化"这个想法)
真值 = 京Q06666 (那 20 张难帧)。
"""
import os, sys, csv, glob
import numpy as np, cv2, torch

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
import locator_text_row as T

D = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images\京Q06666_难"
TRUTH = "京Q06666"
CSV = os.path.join(ROOT, "tools", "out", "replay_base.csv")

from data.load_data import CHARS
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
net = build_net = None
from lprnet_s3_model import build_lprnet_s3
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth"),
                               map_location="cpu"))
net.eval()

def infer(img_crop):
    im = cv2.resize(img_crop, (94, 24)).astype(np.float32)
    im = (im - 127.5) * 0.0078125
    x = torch.from_numpy(im.transpose(2, 0, 1)[None])
    return dec(net(x).numpy()[0])

def crop_box(img, box, margin=0.03):
    H, W = img.shape[:2]
    x0, y0, x1, y1 = box
    mx = (x1 - x0) * margin; my = (y1 - y0) * margin
    x0 = int(max(0, x0 - mx)); y0 = int(max(0, y0 - my))
    x1 = int(min(W, x1 + mx)); y1 = int(min(H, y1 + my))
    if x1 - x0 < 8 or y1 - y0 < 6: return None
    return img[y0:y1, x0:x1]

def new_box(img):
    e = T.preprocess(img, "clahe+sat")
    b = e[:, :, 0].astype(np.int16); r = e[:, :, 2].astype(np.int16)
    m = (((b - r) > 40) & (b > 120)).astype(np.uint8)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    n, lab, st, cen = cv2.connectedComponentsWithStats(m, 8)
    H, W = img.shape[:2]
    best = None
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if a < 0.015 * W * H or h < 12 or w < 30: continue
        ratio = w / float(h)
        if not (2.2 <= ratio <= 4.2): continue
        if best is None or a > best[0]: best = (a, x, y, x + w, y + h)
    return None if best is None else best[1:]

base = {}
with open(CSV, newline="", encoding="utf-8-sig") as fh:
    for row in csv.DictReader(fh):
        base[os.path.basename(row["file"])] = row

files = sorted(glob.glob(os.path.join(D, "*.jpg")))
stat = {"old": 0, "new": 0, "new_enh": 0}
print("%-38s %-14s %-14s %-14s" % ("帧", "现状框", "新框(原图裁)", "新框(增强图裁)"))
for f in files:
    name = os.path.basename(f)
    img = T.imread_u(f)
    enh = T.preprocess(img, "clahe+sat")
    row = base.get(name)
    r_old = "-"
    if row and row["box_x0"] != "":
        cb = crop_box(img, (int(row["box_x0"]), int(row["box_y0"]), int(row["box_x1"]), int(row["box_y1"])))
        r_old = infer(cb) if cb is not None else "-"
    bx = new_box(img)
    r_new = r_enh = "-"
    if bx is not None:
        cb = crop_box(img, bx); r_new = infer(cb) if cb is not None else "-"
        ce = crop_box(enh, bx); r_enh = infer(ce) if ce is not None else "-"
    for k, v in (("old", r_old), ("new", r_new), ("new_enh", r_enh)):
        if v == TRUTH: stat[k] += 1
    print("%-38s %-14s %-14s %-14s" % (name, r_old, r_new, r_enh))
n = len(files)
print("=" * 90)
for k, lab in (("old", "现状框"), ("new", "新框(原图裁)"), ("new_enh", "新框(增强图裁)")):
    print("%-16s 整串全对 %2d/%d (%.0f%%)" % (lab, stat[k], n, 100.0 * stat[k] / n))
