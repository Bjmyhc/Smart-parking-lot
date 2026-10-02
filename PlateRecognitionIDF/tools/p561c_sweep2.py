# -*- coding: utf-8 -*-
"""p561c -- 留白平台 + 失败形态统计 (多字/少字) + v收边对照."""
import os, sys, glob, importlib.util
import numpy as np, cv2, torch
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
spec = importlib.util.spec_from_file_location("p561", os.path.join(ROOT, "tools", "p561_trim_ab.py"))
m = importlib.util.module_from_spec(spec); sys.modules["p561"] = m; spec.loader.exec_module(m)
import locator_replay as R
BASE = m.BASE; SETS = m.SETS
OUT = os.path.join(ROOT, "tools", "out", "p561"); os.makedirs(OUT, exist_ok=True)

COMBOS = []
for pl in (0.02, 0.03, 0.04):
    COMBOS.append(("padL=%.2f padR=0.08 padV=0.08" % pl, pl, 0.08, 0.08, True))
COMBOS.append(("padL=0.03 padR=0.06 padV=0.08", 0.03, 0.06, 0.08, True))
COMBOS.append(("padL=0.03 padR=0.10 padV=0.08", 0.03, 0.10, 0.08, True))
COMBOS.append(("padL=0.03 padR=0.08 padV=0.05", 0.03, 0.08, 0.05, True))
COMBOS.append(("padL=0.03 padR=0.08 padV=0.12", 0.03, 0.08, 0.12, True))
COMBOS.append(("padL=0.03 padR=0.08 padV=0.08 (v\u4e0d\u6536)", 0.03, 0.08, 0.08, False))
COMBOS.append(("padL=0.03 padR=0.08 padV=0.08 (v\u6536\u5230\u5e95=0,1)", 0.03, 0.08, 0.08, True))

res = [{"n": 0, "ok": 0, "extra": 0, "short": 0, "same_len": 0} for _ in COMBOS]
frames = []
for folder, truth in SETS:
    for f in sorted(glob.glob(os.path.join(BASE, folder, "*.jpg"))):
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240: continue
        frames.append((os.path.basename(f), truth, im))
A = m.A
for name, truth, im in frames:
    A.orig_w, A.orig_h = im.shape[1], im.shape[0]
    work, mask, info = R.locate(im.copy(), A)
    ch = info.get("chosen")
    if ch is None or ch.get("fit") is None: continue
    s = im.shape[1] / float(work.shape[1])
    qx = [v * s for v in ch["fit"]["qx"]]; qy = [v * s for v in ch["fit"]["qy"]]
    if COMBOS[-1][4]:
        v_lo, v_hi = m.trim_short(im, qx, qy)
    else:
        v_lo, v_hi = 0.0, 1.0
    for idx, (tag, pl, pr, pv, use_vtrim) in enumerate(COMBOS):
        if tag.endswith("(v\u4e0d\u6536)"):
            vl, vh = 0.0, 1.0
        else:
            vl, vh = v_lo, v_hi
        m.PAD_L = pl; m.PAD_R = pr; m.PAD_V = pv
        blk = m.crop_quad(im, qx, qy, 0.0, 1.0, vl, vh)
        t = m.infer94(blk)
        r = res[idx]; r["n"] += 1
        ok = (t == truth); r["ok"] += ok
        if not ok:
            if len(t) > len(truth): r["extra"] += 1
            elif len(t) < len(truth): r["short"] += 1
            else: r["same_len"] += 1
with open(os.path.join(OUT, "sweep_c.txt"), "w", encoding="utf-8") as fp:
    for idx, (tag, pl, pr, pv, uv) in enumerate(COMBOS):
        r = res[idx]
        line = "%-40s 全对 %3d/%3d (%3.0f%%) | \u9519\u4e2d: \u591a\u5b57 %2d \u5c11\u5b57 %2d \u540c\u957f %2d" % (
            tag, r["ok"], r["n"], 100.0 * r["ok"] / max(1, r["n"]), r["extra"], r["short"], r["same_len"])
        fp.write(line + "\n"); print(line.encode("ascii", "replace").decode())
