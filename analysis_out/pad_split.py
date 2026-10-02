# -*- coding: utf-8 -*-
"""拆分实验: 到底是"补蓝边"还是"重采样变糊"在起作用"""
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

def roundtrip(img, k):
    """只做重采样往返(变糊), 不补边, 回到 94 宽"""
    h, w = img.shape[:2]
    s = cv2.resize(img, (k, h), interpolation=cv2.INTER_AREA)
    return cv2.resize(s, (w, h), interpolation=cv2.INTER_LINEAR)

def padmode(img, k, x0):
    """内容缩到 k 宽, 放到 x0, 其余补牌底蓝"""
    h, w = img.shape[:2]
    s = cv2.resize(img, (k, h), interpolation=cv2.INTER_AREA)
    bg = img[0, 2:12].mean(0)
    c = np.zeros_like(img); c[:, :] = bg
    x0 = max(0, min(w - k, x0)); c[:, x0:x0+k] = s
    return c

def run(files, truth, fn):
    ok = 0; cnt = collections.Counter()
    for f in files:
        img = read(f)
        if img is None: continue
        img = fn(img)
        with torch.no_grad():
            lg = net(torch.from_numpy(prep(img)).unsqueeze(0)).cpu().numpy()[0]
        r = dec(lg, CHARS); cnt[r] += 1
        if r == truth: ok += 1
    return ok, sum(cnt.values()), cnt

FOLDERS = ['京Q-06666', '粤T-666FP', '豫J-7Z921', '豫A-12345']
DATA = {}
for fo in FOLDERS:
    DATA[fo] = (sorted(glob.glob(os.path.join(ROOT, fo, '*.jpg'))), fo.replace('-', ''))

MODES = [('原样', lambda im: im)]
for k in (78, 80, 82, 84, 86, 88, 90):
    MODES.append(('纯重采样 %d/94' % k, (lambda kk: (lambda im: roundtrip(im, kk)))(k)))
    MODES.append(('缩%d居中补蓝' % k, (lambda kk: (lambda im: padmode(im, kk, (94-kk)//2)))(k)))
    MODES.append(('缩%d靠左补蓝' % k, (lambda kk: (lambda im: padmode(im, kk, 0)))(k)))

print('%-20s %s' % ('方案', ' '.join('%10s' % f[:6] for f in FOLDERS) + '   总计'))
for name, fn in MODES:
    cells = []; tot_ok = 0; tot_n = 0
    for fo in FOLDERS:
        files, truth = DATA[fo]
        ok, n, _ = run(files, truth, fn)
        cells.append('%d/%d %4.0f%%' % (ok, n, ok/n*100)); tot_ok += ok; tot_n += n
    print('%-20s %s   %d/%d %4.1f%%' % (name, ' '.join('%10s' % c for c in cells), tot_ok, tot_n, tot_ok/tot_n*100))