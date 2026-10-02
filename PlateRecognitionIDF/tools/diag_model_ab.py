# -*- coding: utf-8 -*-
import os, sys, glob
import numpy as np, cv2, onnxruntime as ort
import torch

ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
LPRNET = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
sys.path.insert(0, os.path.join(ROOT, 'tools')); sys.path.insert(0, LPRNET)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

CROPS = 'G:\\All_Project\\AI_Project\\BY串口助手\\dist\\saved\\images'
BIN = os.path.join(ROOT, 'main', 'models', 'test_input.bin')
blank = len(CHARS) - 1

def dec(arr):
    arr = np.squeeze(np.asarray(arr, dtype=np.float32))
    if arr.shape[0] != len(CHARS): arr = arr.T
    lab = [int(np.argmax(arr[:, j])) for j in range(arr.shape[1])]
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank: out.append(c)
        prev = c
    return ''.join(CHARS[i] for i in out)

def prep(path):
    img = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR)
    img = cv2.resize(img, (94, 24)).astype(np.float32)
    img = (img - 127.5) * 0.0078125
    return img.transpose(2, 0, 1)[None]

# 1) 两个 onnx 各是什么
x = np.fromfile(BIN, dtype='<f4').reshape(24, 94, 3).transpose(2, 0, 1)[None]
for name in ('lprnet_s3.onnx', 'r4_s3.onnx'):
    s = ort.InferenceSession(os.path.join(ROOT, 'tools', 'out', name), providers=['CPUExecutionProvider'])
    o = s.run(None, {s.get_inputs()[0].name: x})[0]
    print('%-16s 读 test_input.bin -> %s' % (name, dec(o[0] if o.ndim == 3 else o)))

# 2) r4 torch 也一样
torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, 'tools', 'out', 'finetune', 'r4_lr1e4_nofreeze.pth'), map_location='cpu'))
net.eval()
print('%-16s 读 test_input.bin -> %s' % ('r4 torch', dec(net(torch.from_numpy(x)).numpy()[0])))

# 3) 两个模型在你自己的实拍裁块上谁更准
sess_old = ort.InferenceSession(os.path.join(ROOT, 'tools', 'out', 'lprnet_s3.onnx'), providers=['CPUExecutionProvider'])
files = []
for d in sorted(os.listdir(CROPS)):
    p = os.path.join(CROPS, d)
    if os.path.isdir(p):
        files += [(f, d.replace('-', '')) for f in glob.glob(os.path.join(p, '*.jpg'))]
files += [(f, '京Q06666') for f in sorted(glob.glob(os.path.join(CROPS, 'IMG_20261001_1740*.jpg')))]
print('\n实拍裁块共 %d 张' % len(files))
ok_old = ok_new = 0
bad = []
for f, gt in files:
    arr = prep(f)
    o = sess_old.run(None, {sess_old.get_inputs()[0].name: arr})[0]
    a = dec(o[0] if o.ndim == 3 else o)
    b = dec(net(torch.from_numpy(arr)).numpy()[0])
    ok_old += (a == gt); ok_new += (b == gt)
    if b != gt:
        bad.append((os.path.basename(f), gt, a, b))
print('旧模型(r4之前) 全对 %d/%d = %.0f%%' % (ok_old, len(files), 100.0*ok_old/len(files)))
print('r4 模型        全对 %d/%d = %.0f%%' % (ok_new, len(files), 100.0*ok_new/len(files)))
print('\nr4 读错的 %d 张 (文件 / 真值 / 旧模型 / r4):' % len(bad))
for r in bad[:20]:
    print('   %-32s %-12s %-12s %s' % r)