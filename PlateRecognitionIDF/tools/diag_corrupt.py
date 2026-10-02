# -*- coding: utf-8 -*-
"""Brute-force which corruption of the crop reproduces the board's output."""
import os, sys, glob
import numpy as np, cv2, torch
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import quant_espdl_s3 as Q
sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

W, H = 94, 24
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\finetune\r4_lr1e4_nofreeze.pth', map_location='cpu'))
net.eval()

def run(img_bgr):
    x = cv2.resize(img_bgr, (W, H)).astype(np.float32)
    x = (x - 127.5) * 0.0078125
    t = torch.from_numpy(x.transpose(2,0,1)[None])
    with torch.no_grad():
        o = net(t).numpy()[0]
    a = np.squeeze(o); lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev, blank = [], 67, len(CHARS)-1
    for c in lab:
        if c != prev and c != blank: res.append(c)
        prev = c
    return ''.join(CHARS[c] for c in res)

CROP_DIR = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
cases = [('IMG_20261001_174038_397174.jpg', '京Q粤'),
         ('IMG_20261001_174040_989107.jpg', '京Q粤'),
         ('IMG_20261001_174043_470367.jpg', '京Q粤粤')]

def variants(img):
    out = {}
    out['原图'] = img
    out['水平翻转'] = cv2.flip(img, 1)
    out['垂直翻转'] = cv2.flip(img, 0)
    out['通道BG<-R'] = img[:, :, [1,2,0]]
    out['只留B三通道'] = np.dstack([img[:,:,0]]*3)
    out['只留R三通道'] = np.dstack([img[:,:,2]]*3)
    for k in (16, 18, 20, 24, 30, 40, 47):
        part = img[:, :k]
        out['左%d列拉伸' % k] = cv2.resize(part, (W, H))
    # 3 列内容重复铺满 (相当于把宽度压缩到 1/3 再还原)
    for k in (31, 47):
        small = cv2.resize(img, (k, H), interpolation=cv2.INTER_AREA)
        out['压到%d列再拉回' % k] = cv2.resize(small, (W, H), interpolation=cv2.INTER_NEAREST)
    # 每 3 列取 1 列
    out['每3列取1列'] = img[:, ::3]
    out['每2列取1列'] = img[:, ::2]
    # 16 字节步长采样: 按字节流看, 相当于 x 方向 3/16 密度
    flat = img.reshape(-1, 3)
    n = W * H
    idx = (np.arange(n) * 16 // 3 * 3 + 0)
    idx = np.clip(idx // 3, 0, flat.shape[0]-1)
    out['字节步长16采样'] = flat[idx].reshape(H, W, 3).astype(np.uint8)
    return out

for fn, board in cases:
    img = cv2.imdecode(np.fromfile(os.path.join(CROP_DIR, fn), np.uint8), cv2.IMREAD_COLOR)
    print('')
    print('=== %s   板端=%s ===' % (fn, board))
    hits = []
    for tag, v in variants(img).items():
        try:
            txt = run(np.ascontiguousarray(v))
        except Exception as e:
            txt = 'ERR %s' % e
        mark = '   <<< 与板端一致' if txt == board else ''
        if txt == board: hits.append(tag)
        print('   %-16s %s%s' % (tag, txt, mark))