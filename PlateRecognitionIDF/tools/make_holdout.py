# -*- coding: utf-8 -*-
"""把 finetune_s3.py 里那份"实拍留出集"原样复制到 out/holdout —— 好单独量它的 int8 精度。

为什么: 实拍 165 张里有 132 张是训练用的, 直接在整份上打分是"开卷考试"。
      留出那 33 张才是闭卷。复制出来(按车牌分子目录, 标签靠文件夹名)就能单独测。
"""
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
    ap.add_argument('--data-dirs', default=r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images')
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