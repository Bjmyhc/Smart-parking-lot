# -*- coding: utf-8 -*-
"""竖直方向扫描: 框"偏高"(比例 2.5 vs 真牌 3.14) => 字符被竖直压扁。
   模拟"把上下多出来的边收掉"(取中间若干行再拉伸回 24 行)对准确率的影响。"""
import os, sys, glob
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

def vsqueeze(img, p):
    """把内容缩到 24*(1-p) 行并居中, 其余补牌底蓝  == 取样框上下各收 p/2"""
    if p <= 0: return img
    k = max(6, int(round(24 * (1 - p))))
    small = cv2.resize(img, (94, k), interpolation=cv2.INTER_AREA)
    bg = img[:, 1:8].reshape(-1, 3).mean(0)
    c = np.zeros_like(img); c[:, :] = bg
    y0 = (24 - k) // 2
    c[y0:y0+k, :] = small
    return c

FOLDERS = ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']
DATA = {fo: (sorted(glob.glob(os.path.join(ROOT, fo, '*.jpg'))), fo.replace('-', '')) for fo in FOLDERS}

print('%-12s %s' % ('上下各收', ' '.join('%10s' % f[:6] for f in FOLDERS) + '   总计'))
for p in (0.0, 0.08, 0.16, 0.20, 0.25, 0.30, 0.38, 0.50, -0.08, -0.20):
    cells = []; to = tn = 0
    for fo in FOLDERS:
        files, truth = DATA[fo]; ok = 0
        for f in files:
            img = read(f)
            if img is None: continue
            img = vsqueeze(img, p)
            with torch.no_grad():
                lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
            if dec(lg, CHARS) == truth: ok += 1
        n = len(files)
        cells.append('%d/%d %3.0f%%' % (ok, n, ok/n*100)); to += ok; tn += n
    print('%-12s %s   %d/%d %4.1f%%' % ('%+.0f%%' % (p*100), ' '.join('%10s' % c for c in cells), to, tn, to/tn*100))