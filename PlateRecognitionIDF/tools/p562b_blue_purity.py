# --- 可移植路径自举 (Tool/relocate_paths.py 注入, 勿手改) -------------------
import os as _os

_HERE_ = _os.path.dirname(_os.path.abspath(__file__))


def _find_repo_root_(start):
    """从 start 逐级上溯, 找到同时含 Tool/ 与 Node/ 的目录 = 仓库根。"""
    cur = start
    while True:
        if _os.path.isdir(_os.path.join(cur, 'Tool')) and \
           _os.path.isdir(_os.path.join(cur, 'Node')):
            return cur
        parent = _os.path.dirname(cur)
        if parent == cur:
            return _os.path.dirname(start)
        cur = parent


_REPO_ROOT_ = _find_repo_root_(_HERE_)
_AI_ROOT_ = _os.path.dirname(_REPO_ROOT_)
# --- 自举结束 ---------------------------------------------------------------

# -*- coding: utf-8 -*-
import os, glob, numpy as np, cv2
BASE = _AI_ROOT_ + r'\BY串口助手\dist\saved\images'
SETS = ["\u4eacQ06666_\u96be", "\u8c6bFSQ818_\u96be", "\u8c6bA8F8Q8_\u96be"]
ROI_MIN_VALUE, BR = 120.0, 40.0
def load(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
rows = []
for s in SETS:
    fs = sorted(glob.glob(os.path.join(BASE, s, "*")))
    fs = fs[::max(1, len(fs)//25)]
    for p in fs:
        im = load(p)
        if im is None: continue
        f = im.astype(np.int32)
        b, g, r = f[:,:,0], f[:,:,1], f[:,:,2]
        v = f.max(axis=2)
        mask = (v >= ROI_MIN_VALUE) & ((b - r) > BR) & (b > ROI_MIN_VALUE)
        n = mask.sum()
        if n == 0:
            rows.append((s, os.path.basename(p), 0, 0, 0, 0, 0)); continue
        vv = v[mask]; mm = f.min(axis=2)[mask]
        s255 = 255.0 * (vv - mm) / np.maximum(vv, 1)
        dark = ((vv < 170) & (s255 < 90)).mean()
        sat_hi = (s255 >= 120).mean()
        rows.append((s, os.path.basename(p), im.shape[1], n, float(n)/(im.shape[0]*im.shape[1])*100, dark*100, sat_hi*100))
out = []
hdr = "set|file|W|mask_px|mask%%|dark_desat%%|sat>=120%%"
out.append(hdr)
for x in rows:
    out.append("%s|%s|%s|%d|%.2f|%.1f|%.1f" % x)
open(_REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\p562\blue_purity.txt',"w",encoding="utf-8").write("\n".join(out))
# aggregate
import statistics as st
vals = [x for x in rows if x[3] > 0]
print("frames analysed =", len(vals))
print("median mask%% of frame = %.2f" % st.median([v[4] for v in vals]))
print("median dark&desat share of mask = %.1f%%" % st.median([v[5] for v in vals]))
print("median high-sat share of mask = %.1f%%" % st.median([v[6] for v in vals]))
print("mean dark&desat share = %.1f%%" % (sum(v[5] for v in vals)/len(vals)))
