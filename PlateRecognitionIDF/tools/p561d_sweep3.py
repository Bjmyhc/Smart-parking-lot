# -*- coding: utf-8 -*-
"""p561d -- padV 继续往上扫 + 分车牌交叉验证(防过拟合)."""
import os, sys, glob, importlib.util
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
spec = importlib.util.spec_from_file_location("p561", os.path.join(ROOT, "tools", "p561_trim_ab.py"))
m = importlib.util.module_from_spec(spec); sys.modules["p561"] = m; spec.loader.exec_module(m)
import locator_replay as R
BASE = m.BASE; SETS = m.SETS
OUT = os.path.join(ROOT, "tools", "out", "p561")
COMBOS = []
for pv in (0.08, 0.12, 0.15, 0.18, 0.22):
    COMBOS.append(("padL=0.03 padR=0.08 padV=%.2f" % pv, pv))
COMBOS.append(("padL=0.04 padR=0.08 padV=0.15", 0.15))
COMBOS.append(("padL=0.02 padR=0.08 padV=0.15", 0.15))
res = [{"n": 0, "ok": 0} for _ in COMBOS]
per = [dict() for _ in COMBOS]
frames = []
for folder, truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240: continue
        frames.append((folder, os.path.basename(f), truth, im))
A = m.A
for folder, name, truth, im in frames:
    A.orig_w, A.orig_h = im.shape[1], im.shape[0]
    work, mask, info = R.locate(im.copy(), A)
    ch = info.get("chosen")
    if ch is None or ch.get("fit") is None: continue
    s = im.shape[1] / float(work.shape[1])
    qx = [v * s for v in ch["fit"]["qx"]]; qy = [v * s for v in ch["fit"]["qy"]]
    v_lo, v_hi = m.trim_short(im, qx, qy)
    for idx, (tag, pv) in enumerate(COMBOS):
        pl = 0.03; pr = 0.08
        if tag.startswith("padL=0.04"): pl = 0.04
        if tag.startswith("padL=0.02"): pl = 0.02
        m.PAD_L = pl; m.PAD_R = pr; m.PAD_V = pv
        blk = m.crop_quad(im, qx, qy, 0.0, 1.0, v_lo, v_hi)
        t = m.infer94(blk)
        r = res[idx]; r["n"] += 1; r["ok"] += (t == truth)
        d = per[idx].setdefault(folder, [0, 0]); d[0] += (t == truth); d[1] += 1
with open(os.path.join(OUT, "sweep_d.txt"), "w", encoding="utf-8") as fp:
    for idx, (tag, pv) in enumerate(COMBOS):
        r = res[idx]
        det = "  ".join("%s %d/%d" % (k, v[0], v[1]) for k, v in per[idx].items())
        line = "%-38s 全对 %3d/%3d (%3.0f%%) | %s" % (
            tag, r["ok"], r["n"], 100.0 * r["ok"] / max(1, r["n"]), det)
        fp.write(line + "\n"); print(line.encode("ascii", "replace").decode())
