# -*- coding: utf-8 -*-
"""用"真正部署的 r4 模型"重跑 17:40 那批裁块 —— 之前 diag_crops.py 用的是旧 ONNX。"""
import os, sys, glob
import numpy as np
import torch
import cv2

ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
LPRNET = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

CROPS = 'G:\\All_Project\\AI_Project\\BY串口助手\\dist\\saved\\images'
WEIGHTS = os.path.join(ROOT, 'tools', 'out', 'finetune', 'r4_lr1e4_nofreeze.pth')

torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(WEIGHTS, map_location='cpu'))
net.eval()

blank = len(CHARS) - 1
def decode(arr):
    arr = np.squeeze(arr)
    if arr.shape[0] != len(CHARS):
        arr = arr.T
    lab = [int(np.argmax(arr[:, j])) for j in range(arr.shape[1])]
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank: out.append(c)
        prev = c
    return ''.join(CHARS[i] for i in out)

files = sorted(glob.glob(os.path.join(CROPS, 'IMG_20261001_1740*.jpg')))
print('找到 %d 张裁块' % len(files))
print('%-34s %-16s %s' % ('crop', 'r4(torch)', '板端当年实时读数'))
print('-' * 96)
board = ['京Q粤','京粤Q粤浙粤','粤京粤','京Q粤','京粤','京粤粤粤','京Q粤粤','浙京Q粤粤粤','粤京粤粤','粤浙粤浙粤浙粤浙粤']
for f, b in zip(files, board):
    img = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
    img = cv2.resize(img, (94, 24)).astype(np.float32)
    img = (img - 127.5) * 0.0078125
    t = torch.from_numpy(np.ascontiguousarray(img.transpose(2, 0, 1))).unsqueeze(0)
    txt = decode(net(t).numpy()[0])
    print('%-34s %-16s %s   %s' % (os.path.basename(f), txt, b, '== 一致' if txt == b else ''))