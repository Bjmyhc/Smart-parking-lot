# -*- coding: utf-8 -*-
"""p561b_pad_sweep.py -- 收边关掉之后, 左右留白取多少最好 + 半收对照."""
import os, sys, glob, argparse
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import importlib.util
spec = importlib.util.spec_from_file_location("p561", os.path.join(ROOT, "tools", "p561_trim_ab.py"))
m = importlib.util.module_from_spec(spec)
sys.modules["p561"] = m
spec.loader.exec_module(m)
import locator_replay as R
cv2 = m.cv2; np = m.np

BASE = m.BASE
SETS = m.SETS
OUT = os.path.join(ROOT, "tools", "out", "p561"); os.makedirs(OUT, exist_ok=True)

VARIANTS = [
    ("A u=0 padL+1% R+8% (=\u5173\u6536\u8fb9\u73b0\u72b6)", 0, 0.01, 0.08),
    ("B u=0 padL+3% R+8%", 0, 0.03, 0.08),
    ("C u=0 padL-1% R+8%", 0, -0.01, 0.08),
    ("D u=0 padL+1% R+5%", 0, 0.01, 0.05),
    ("E u=0 padL+1% R+12%", 0, 0.01, 0.12),
    ("F u=0 padL+5% R+8%", 0, 0.05, 0.08),
    ("G \u534a\u6536 c_lo/2", -1, 0.01, 0.08),
]
res = [{"n": 0, "ok": 0, "first": 0} for _ in VARIANTS]

def crop_pad(im, qx, qy, u_lo, u_hi, v_lo, v_hi, pad_l, pad_r):
    m.PAD_L = pad_l; m.PAD_R = pad_r
    return m.crop_quad(im, qx, qy, u_lo, u_hi, v_lo, v_hi)

frames = []
for folder, truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240:
            continue
        frames.append((folder, os.path.basename(f), truth, im))

A = m.A
for folder, name, truth, im in frames:
    A.orig_w, A.orig_h = im.shape[1], im.shape[0]
    work, mask, info = R.locate(im.copy(), A)
    ch = info.get("chosen")
    if ch is None or ch.get("fit") is None:
        continue
    s = im.shape[1] / float(work.shape[1])
    qx = [v * s for v in ch["fit"]["qx"]]; qy = [v * s for v in ch["fit"]["qy"]]
    v_lo, v_hi = m.trim_short(im, qx, qy)
    uinfo = m.probe_u(im, qx, qy, v_lo, v_hi)
    c_lo = uinfo["c_lo"] if uinfo["c_lo"] >= 0 else 0.0
    for idx, (tag, mode, pl, pr) in enumerate(VARIANTS):
        if mode == -1:
            u_lo = (c_lo / 2.0) if uinfo["ok"] else 0.0
        else:
            u_lo = c_lo if (mode == 1 and uinfo["ok"]) else 0.0
        blk = crop_pad(im, qx, qy, u_lo, 1.0, v_lo, v_hi, pl, pr)
        t = m.infer94(blk)
        res[idx]["n"] += 1
        res[idx]["ok"] += (t == truth)
        res[idx]["first"] += (len(t) > 0 and t[0] == truth[0])

with open(os.path.join(OUT, "pad_sweep.txt"), "w", encoding="utf-8") as fp:
    for idx, (tag, mode, pl, pr) in enumerate(VARIANTS):
        r = res[idx]
        line = "%-42s 整串全对 %3d/%3d (%3.0f%%)  首字对 %3d/%3d (%3.0f%%)" % (
            tag, r["ok"], r["n"], 100.0 * r["ok"] / max(1, r["n"]),
            r["first"], r["n"], 100.0 * r["first"] / max(1, r["n"]))
        fp.write(line + "\n")
        print(line.encode("ascii", "replace").decode())
