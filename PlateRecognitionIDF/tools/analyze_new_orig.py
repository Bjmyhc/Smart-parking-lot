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

import sys, os
sys.argv = ['x']
import numpy as np, cv2
root = _AI_ROOT_ + r'\BY串口助手\dist\saved\images'
for d in sorted(os.listdir(root)):
    p = os.path.join(root, d)
    if not os.path.isdir(p):
        continue
    fs = sorted(f for f in os.listdir(p) if f.lower().endswith('.jpg'))
    if not fs:
        continue
    rows = []
    for f in fs:
        img = cv2.imdecode(np.fromfile(os.path.join(p, f), np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            continue
        if img.shape[0] != 240:
            continue
        b, g, r = img[:, :, 0].astype(np.int32), img[:, :, 1].astype(np.int32), img[:, :, 2].astype(np.int32)
        lum = (r * 299 + g * 587 + b * 114) // 1000
        blown = float(np.mean((r >= 235) & (g >= 235) & (b >= 235))) * 100
        blueness = b - r
        blue = blueness >= 40
        nblue = int(blue.sum())
        rows.append((f, float(lum.mean()), blown, nblue, float(np.median(blueness[blue])) if nblue > 0 else float('nan')))
    if not rows:
        continue
    print('==', d, len(rows), 'frames')
    lum = np.array([x[1] for x in rows]); bl = np.array([x[2] for x in rows])
    nb = np.array([x[3] for x in rows]); mb = np.array([x[4] for x in rows])
    print('   lum  mean %.0f  min %.0f  max %.0f' % (lum.mean(), lum.min(), lum.max()))
    print('   blown%% mean %.1f  min %.1f  max %.1f' % (bl.mean(), bl.min(), bl.max()))
    print('   blue px mean %.0f  min %d  max %d   med(b-r) of blue mean %.0f' % (nb.mean(), nb.min(), nb.max(), np.nanmean(mb)))
    print('   per-frame: ' + ' '.join('%d/%.0f/%.0f' % (x[3], x[1], x[2]) for x in rows[:30]))
    print()

