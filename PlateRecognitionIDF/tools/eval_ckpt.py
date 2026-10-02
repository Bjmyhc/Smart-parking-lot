# -*- coding: utf-8 -*-
"""给一份 LPRNet 权重打分 —— 在几套验证集上、错成什么样, 一屏看完。

为什么需要这个
--------------
微调赚没赚, 不能只看一个数:

  * 实拍留出那 33 张和训练集是**同 4 块牌**, 量的是"记住这几块牌的样子" (记忆)
  * official_val (皖 200 张 94x24) 量的是"原来那份通用能力还在不在" (遗忘)
  * ccpd_plates/val (794 张, 31 个省) 量的是"省份字认识多少个" (泛化)

三个一起看, 才分得清"变强"和"变偏"。

用法
----
  python eval_ckpt.py --weights <lprnet>/weights/Final_LPRNet_model.pth
  python eval_ckpt.py --weights tools/out/finetune/r4_xxx.pth --sets real,officialval,ccpdval
"""

import argparse
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
LPRNET = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
REAL = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
IMG_W, IMG_H = 94, 24


def _clean(s):
    return s.split('_')[0].replace('-', '').strip()


def label_of(path):
    name = os.path.splitext(os.path.basename(path))[0]
    cand = _clean(name)
    if len(cand) >= 6:
        return cand
    return _clean(os.path.basename(os.path.dirname(os.path.abspath(path))))


def load_bgr(path):
    import cv2
    import numpy as np
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def to_tensor(img):
    import numpy as np
    im = img.astype(np.float32)
    im = (im - 127.5) / 128.0
    return im.transpose(2, 0, 1)


def collect_dir(d):
    out = []
    for root, _dirs, files in os.walk(d):
        for f in files:
            if os.path.splitext(f)[1].lower() in ('.jpg', '.jpeg', '.png'):
                out.append(os.path.join(root, f))
    return sorted(out)


def decode(logits, CHARS):
    seq = logits.argmax(0)
    res, prev = [], -1
    for c in seq:
        if c != prev and c != len(CHARS) - 1:
            res.append(CHARS[c])
        prev = c
    return ''.join(res)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--weights', default=os.path.join(LPRNET, 'weights', 'Final_LPRNet_model.pth'))
    ap.add_argument('--sets', default='real,officialval,ccpdval')
    ap.add_argument('--show-wrong', type=int, default=20)
    a = ap.parse_args()

    if LPRNET not in sys.path:
        sys.path.insert(0, LPRNET)
    if _HERE not in sys.path:
        sys.path.insert(0, _HERE)

    import cv2
    import numpy as np
    import torch
    from data.load_data import CHARS, CHARS_DICT
    from model.LPRNet import build_lprnet

    net = build_lprnet(lpr_max_len=8, phase=False, class_num=len(CHARS), dropout_rate=0.5)
    net.load_state_dict(torch.load(a.weights, map_location='cpu'))
    net.eval()
    print('权重: %s' % a.weights)

    pool = {
        'real': collect_dir(REAL),
        'officialval': collect_dir(os.path.join(LPRNET, 'data', 'official_val')),
        'ccpdval': collect_dir(os.path.join(LPRNET, 'data', 'ccpd_plates', 'val')),
    }

    for name in a.sets.split(','):
        name = name.strip()
        if name not in pool:
            print('!! 不认识的验证集: %s' % name)
            continue
        paths = [p for p in pool[name] if all(c in CHARS_DICT for c in label_of(p))]
        if not paths:
            print('!! %s 是空的' % name)
            continue
        ok = 0
        wrong = []
        per = defaultdict(lambda: [0, 0])      # 车牌 -> [对, 共]
        prov = defaultdict(lambda: [0, 0])     # 省份字 -> [对, 共]
        for p in paths:
            img = load_bgr(p)
            if img is None:
                continue
            if img.shape[1] != IMG_W or img.shape[0] != IMG_H:
                img = cv2.resize(img, (IMG_W, IMG_H))
            x = torch.from_numpy(np.ascontiguousarray(to_tensor(img)))[None]
            with torch.no_grad():
                lg = net(x)[0].numpy()
            got = decode(lg, CHARS)
            truth = label_of(p)
            per[truth][1] += 1
            prov[truth[:1]][1] += 1
            if got == truth:
                ok += 1
                per[truth][0] += 1
                prov[truth[:1]][0] += 1
            else:
                wrong.append((p, truth, got))
        n = len(paths)
        print('')
        print('===== %s : %d/%d = %.1f%% =====' % (name, ok, n, 100.0 * ok / n))
        if name == 'real':
            print('  按车牌:')
            for k in sorted(per, key=lambda t: -per[t][1]):
                o, c = per[k]
                print('    %-10s %3d/%3d  %5.1f%%' % (k, o, c, 100.0 * o / c))
        else:
            print('  按省份字(前 12, 按数量):')
            for k in sorted(prov, key=lambda t: -prov[t][1])[:12]:
                o, c = prov[k]
                print('    %-4s %3d/%3d  %5.1f%%' % (k, o, c, 100.0 * o / c))
        if wrong and a.show_wrong > 0:
            print('  错例(最多 %d):' % a.show_wrong)
            for p, truth, got in wrong[:a.show_wrong]:
                print('    %-28s 真=%-9s 认成=%s' % (os.path.basename(p), truth, got))


if __name__ == '__main__':
    main()