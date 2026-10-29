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

import re, io, os
p = _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\p561\detail.txt'
rev = []; ok = []; nobox = 0
row = 0
for ln in io.open(p, encoding="utf-8"):
    ln = ln.rstrip("\n")
    if "  " not in ln and ln.strip() == "": continue
    m = re.search(r"\| u_ok=(\w) c_lo=([\d.]+) c_hi=([\d.]+) band=([\d.]+)", ln)
    if not m: 
        if "NO-BOX" in ln: nobox += 1
        continue
    row += 1
    parts = ln.split("|| ")[-1]
    d = {}
    for tok in parts.split("  "):
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    t = ln.split("\u771f\u503c ")[1].split(" |")[0]
    v0 = d.get("V0", "").rstrip("*"); v1 = d.get("V1", "").rstrip("*")
    if v0 == t and v1 != t: rev.append(ln)
    if v0 == t and v1 == t: ok.append(ln)
print("rows=%d nobox=%d  both_right=%d  V0right_V1wrong=%d" % (row, nobox, len(ok), len(rev)))
for ln in rev[:20]: print(ln)
