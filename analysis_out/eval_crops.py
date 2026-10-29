# -*- coding: utf-8 -*-
"""把串口助手存下来的 94x24 模型输入块, 用浮点模型重跑一遍, 按文件夹(真值)统计"""
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

import os, sys, glob, collections, json
import numpy as np, cv2, torch

LPR = _AI_ROOT_ + r'\LPRNet_Pytorch'
sys.path.insert(0, LPR)
os.chdir(LPR)
from data.load_data import CHARS
from model.LPRNet import build_lprnet

ROOT = _AI_ROOT_ + r'\BY串口助手\dist\saved\images'

def greedy_decode(preb, chars):
    blank = len(chars) - 1
    ids = [int(np.argmax(preb[:, j], axis=0)) for j in range(preb.shape[1])]
    out, pre = [], ids[0]
    if pre != blank: out.append(pre)
    for c in ids:
        if (pre == c) or (c == blank):
            if c == blank: pre = c
            continue
        out.append(c); pre = c
    return ''.join(chars[i] for i in out)

net = build_lprnet(lpr_max_len=8, phase=False, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(LPR, 'weights', 'Final_LPRNet_model.pth'), map_location='cpu'))
net.eval()

def read_bgr(p):
    return cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)

def prep(img):
    img = cv2.resize(img, (94, 24)).astype('float32')
    img -= 127.5; img *= 0.0078125
    return np.transpose(img, (2, 0, 1))

report = {}
for folder in sorted(os.listdir(ROOT)):
    d = os.path.join(ROOT, folder)
    if not os.path.isdir(d): continue
    truth = folder.replace('-', '')
    files = sorted(glob.glob(os.path.join(d, '*.jpg')))
    cnt = collections.Counter()
    for f in files:
        img = read_bgr(f)
        if img is None: continue
        t = torch.from_numpy(prep(img)).unsqueeze(0)
        with torch.no_grad():
            logits = net(t).cpu().numpy()[0]
        cnt[greedy_decode(logits, CHARS)] += 1
    n = sum(cnt.values())
    ok = cnt.get(truth, 0)
    report[folder] = dict(truth=truth, n=n, ok=ok, acc=round(ok / n * 100, 1) if n else 0,
                          top=cnt.most_common(12))
    print('=== %s (真值 %s) 共 %d 张, 全对 %d (%.1f%%)' % (folder, truth, n, ok, ok / n * 100 if n else 0))
    for k, v in cnt.most_common(12):
        print('     %-14s %d' % (k, v))
print(json.dumps(report, ensure_ascii=False))