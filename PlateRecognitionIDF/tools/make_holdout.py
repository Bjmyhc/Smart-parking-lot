# -*- coding: utf-8 -*-
"""把 finetune_s3.py 里那份"实拍留出集"原样复制到 out/holdout —— 好单独量它的 int8 精度。

为什么: 实拍 165 张里有 132 张是训练用的, 直接在整份上打分是"开卷考试"。
      留出那 33 张才是闭卷。复制出来(按车牌分子目录, 标签靠文件夹名)就能单独测。
"""
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

import argparse
import os
import shutil
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from finetune_s3 import collect, label_of, split_holdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-dirs', default=_AI_ROOT_ + r'\BY串口助手\dist\saved\images')
    ap.add_argument('--out', default=os.path.join(_HERE, 'out', 'holdout'))
    a = ap.parse_args()

    shoot = []
    for d in sorted(os.listdir(a.data_dirs)):
        sub = os.path.join(a.data_dirs, d)
        if os.path.isdir(sub):
            shoot += collect([sub])
    _tr, hold = split_holdout(shoot, 5)
    for p in hold:
        dd = os.path.join(a.out, label_of(p))
        os.makedirs(dd, exist_ok=True)
        shutil.copy2(p, os.path.join(dd, os.path.basename(p)))
    print('留出 %d 张 -> %s' % (len(hold), a.out))
    print(dict(Counter(label_of(p) for p in hold)))


if __name__ == '__main__':
    main()