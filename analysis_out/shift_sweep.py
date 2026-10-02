# -*- coding: utf-8 -*-
"""只做水平平移(不缩放): 看内容在 94px 里偏左/偏右对准确率的影响"""
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
    img = cv2.resize(img, (94, 24)).astype('float32')
    img -= 127.5; img *= 0.0078125
    return np.transpose(img, (2, 0, 1))

def shift(img, dx):
    """内容右移 dx>0: 左边补牌底蓝, 右边溢出裁掉"""
    h, w = img.shape[:2]
    if dx == 0: return img
    c = np.zeros_like(img); c[:, :] = img[0, 2:12].mean(0)
    if dx > 0:
        c[:, dx:] = img[:, :w-dx]
    else:
        c[:, :w+dx] = img[:, -dx:]
    return c

FOLDERS = ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']
DATA = {fo: (sorted(glob.glob(os.path.join(ROOT, fo, '*.jpg'))), fo.replace('-', '')) for fo in FOLDERS}

print('%-14s %s' % ('平移', ' '.join('%10s' % f[:6] for f in FOLDERS) + '   总计'))
for dx in (-4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6):
    cells = []; to = 0; tn = 0
    for fo in FOLDERS:
        files, truth = DATA[fo]
        ok = 0; n = 0
        for f in files:
            img = read(f)
            if img is None: continue
            img = shift(img, dx)
            with torch.no_grad():
                lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
            if dec(lg, CHARS) == truth: ok += 1
            n += 1
        cells.append('%d/%d %3.0f%%' % (ok, n, ok/n*100)); to += ok; tn += n
    print('右移 %+d px      %s   %d/%d %4.1f%%' % (dx, ' '.join('%10s' % c for c in cells), to, tn, to/tn*100))