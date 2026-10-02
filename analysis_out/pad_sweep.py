# -*- coding: utf-8 -*-
"""对照实验: 给 94x24 模型输入 左右补蓝边(模拟"取样框两端多留白"), 看准确率怎么走"""
import os, sys, glob, collections
import numpy as np, cv2, torch

LPR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
sys.path.insert(0, LPR); os.chdir(LPR)
from data.load_data import CHARS
from model.LPRNet import build_lprnet

ROOT = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'

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

def read_bgr(p): return cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)

def pad_sim(img, pl, pr):
    """把原 94 宽内容缩到 (94*(1-pl-pr)) 宽, 左右各补占位比例的'牌底蓝'"""
    h, w = img.shape[:2]
    keep = max(8, int(round(w * (1.0 - pl - pr))))
    small = cv2.resize(img, (keep, h), interpolation=cv2.INTER_AREA)
    # 牌底色: 取四角附近的均值(避开车牌边框)
    bg = np.array([img[0, 2:12].mean(0), img[-1, 2:12].mean(0)]).mean(0)
    canvas = np.zeros_like(img); canvas[:, :] = bg
    x0 = int(round((w - keep) * (pl / (pl + pr)))) if (pl + pr) > 0 else 0
    x0 = max(0, min(w - keep, x0))
    canvas[:, x0:x0 + keep] = small
    return canvas

def prep(img):
    img = cv2.resize(img, (94, 24)).astype('float32')
    img -= 127.5; img *= 0.0078125
    return np.transpose(img, (2, 0, 1))

def run(files, truth, pl, pr):
    ok = 0; cnt = collections.Counter()
    for f in files:
        img = read_bgr(f)
        if img is None: continue
        if pl or pr: img = pad_sim(img, pl, pr)
        with torch.no_grad():
            lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
        r = greedy_decode(lg, CHARS); cnt[r] += 1
        if r == truth: ok += 1
    n = sum(cnt.values())
    return n, ok, cnt

GRID = [(0,0), (0,0.04), (0,0.08), (0,0.12), (0.04,0.08), (0.06,0.10), (0.04,0.04), (0.08,0.08), (0.10,0.10)]
for folder in ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']:
    d = os.path.join(ROOT, folder); truth = folder.replace('-', '')
    files = sorted(glob.glob(os.path.join(d, '*.jpg')))
    print('=== %s (真值 %s, %d 张)' % (folder, truth, len(files)))
    for pl, pr in GRID:
        n, ok, cnt = run(files, truth, pl, pr)
        print('   左%4.0f%% 右%4.0f%% -> 全对 %2d/%2d (%5.1f%%)  最常见: %s' % (
            pl*100, pr*100, ok, n, ok/n*100, ' '.join('%s x%d' % (k, v) for k, v in cnt.most_common(3))))