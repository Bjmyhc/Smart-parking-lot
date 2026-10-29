# -*- coding: utf-8 -*-
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

import os, glob, collections
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
roots = [
 _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out',
 _REPO_ROOT_ + r'\Doc\硬件资料\车牌测试样例',
 _AI_ROOT_ + r'\LPRNet_Pytorch\data\esp32cam',
]
for r in roots:
    if not os.path.isdir(r): print("MISSING", r); continue
    print("=== ", r)
    for d in sorted(os.listdir(r)):
        sub = os.path.join(r, d)
        if not os.path.isdir(sub): continue
        fs = [f for f in os.listdir(sub) if f.lower().endswith((".jpg",".jpeg",".png"))]
        if not fs: continue
        im = load(os.path.join(sub, fs[0]))
        print("  %-16s n=%4d  例子尺寸 %s" % (d, len(fs), (im.shape if im is not None else None)))
    fs = [f for f in os.listdir(r) if f.lower().endswith((".jpg",".jpeg",".png"))]
    if fs:
        im = load(os.path.join(r, fs[0]))
        print("  (根目录) n=%d 例子 %s %s" % (len(fs), fs[0], im.shape if im is not None else None))
