# -*- coding: utf-8 -*-
"""稳定性检验: 奇偶分半, 看最优外扩值是否复现"""
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
def apply_p(img, p, anchor):
    s = 1.04 / (1.04 + p); k = max(8, int(round(94 * s)))
    small = cv2.resize(img, (k, 24), interpolation=cv2.INTER_AREA)
    c = np.zeros_like(img); c[:, :] = img[0, 2:12].mean(0)
    x0 = (94 - k) if anchor == 'right' else (94 - k) // 2
    c[:, x0:x0+k] = small
    return c
FOLDERS = ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']
DATA = {fo: (sorted(glob.glob(os.path.join(ROOT, fo, '*.jpg'))), fo.replace('-', '')) for fo in FOLDERS}
for anchor in ('right', 'center'):
    print('--- %s ---' % anchor)
    print('%-8s %-9s %-9s' % ('p', '奇数帧', '偶数帧'))
    for p in (0.0, 0.01, 0.02, 0.03, 0.04, 0.05, 0.06):
        res = []
        for par in (0, 1):
            to = tn = 0
            for fo in FOLDERS:
                files, truth = DATA[fo]
                sub = files[par::2]
                ok = 0
                for f in sub:
                    img = read(f)
                    if img is None: continue
                    if p > 0: img = apply_p(img, p, anchor)
                    with torch.no_grad():
                        lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
                    if dec(lg, CHARS) == truth: ok += 1
                to += ok; tn += len(sub)
            res.append('%d/%-3d %4.0f%%' % (to, tn, to/tn*100))
        print('+%-7.0f%% %-9s %-9s' % (p*100, res[0], res[1]))