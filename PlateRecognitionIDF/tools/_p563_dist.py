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

import os, collections, glob
L = _AI_ROOT_ + r'\LPRNet_Pytorch\data'
for sub in ["official_train", "official_val", "ccpd_plates"]:
    d = os.path.join(L, sub)
    if not os.path.isdir(d):
        print(sub, "-> missing"); continue
    fs = []
    for root, dirs, files in os.walk(d):
        for f in files:
            if f.lower().endswith((".jpg",".png",".jpeg")): fs.append(f)
        if len(fs) > 40000: break
    c = collections.Counter(f[0] for f in fs if f)
    print("%-14s n=%6d  top10: %s" % (sub, len(fs), " ".join("%s%d" % (k,v) for k,v in c.most_common(10))))
print()
d2 = _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\collect2'
for root, dirs, files in os.walk(d2):
    print(root, "->", len([f for f in files if f.lower().endswith(('.jpg','.png'))]))
