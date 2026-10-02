# -*- coding: utf-8 -*-
"""真实改法模拟: 增大"长边左端外扩" p  => 旧图缩放 1.04/(1.04+p) 并右锚定"""
import os, sys, glob, collections
import numpy as np, cv2, torch

LPR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
sys.path.insert(0, LPR); os.chdir(LPR)
from data.load_data import CHARS
from model.LPRNet import build_lprnet
ROOT = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'

def dec(p_, chars):
    blank = len(chars) - 1
    ids = [int(np.argmax(p_[:, j], axis=0)) for j in range(p_.shape[1])]
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
read = lambda p: cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)

def prep(img):
    img = cv2.resize(img, (94, 24)).astype('float32'); img -= 127.5; img *= 0.0078125
    return np.transpose(img, (2, 0, 1))

def apply_p(img, p, anchor='right'):
    """旧 ROI 宽 1.04W; 新 ROI 宽 (1.04+p)W。返回新 ROI 下的 94x24"""
    s = 1.04 / (1.04 + p)
    k = max(8, int(round(94 * s)))
    small = cv2.resize(img, (k, 24), interpolation=cv2.INTER_AREA)
    c = np.zeros_like(img); c[:, :] = img[0, 2:12].mean(0)
    if anchor == 'right': x0 = 94 - k
    elif anchor == 'center': x0 = (94 - k) // 2
    else: x0 = 0
    c[:, x0:x0+k] = small
    return c

FOLDERS = ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']
DATA = {fo: (sorted(glob.glob(os.path.join(ROOT, fo, '*.jpg'))), fo.replace('-', '')) for fo in FOLDERS}

for anchor in ('right', 'center', 'left'):
    print('--- 锚定: %s ---' % anchor)
    print('%-10s %s' % ('p', ' '.join('%10s' % f[:6] for f in FOLDERS) + '   总计'))
    for p in (0.0, 0.01, 0.02, 0.03, 0.04, 0.05, 0.06):
        cells = []; to = 0; tn = 0
        for fo in FOLDERS:
            files, truth = DATA[fo]; ok = 0; n = 0
            for f in files:
                img = read(f)
                if img is None: continue
                if p > 0: img = apply_p(img, p, anchor)
                with torch.no_grad():
                    lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
                if dec(lg, CHARS) == truth: ok += 1
                n += 1
            cells.append('%d/%d %3.0f%%' % (ok, n, ok/n*100)); to += ok; tn += n
        print('%-10s %s   %d/%d %4.1f%%' % ('+%.0f%%' % (p*100), ' '.join('%10s' % c for c in cells), to, tn, to/tn*100))