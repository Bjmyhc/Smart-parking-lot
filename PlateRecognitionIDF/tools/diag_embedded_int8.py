# -*- coding: utf-8 -*-
"""决定性对照: 把板端内嵌样张 test_input.bin 同时喂给
   (a) PC float (torch r4)
   (b) PC int8 (esp-ppq 量化图, 复刻部署配方: r4 权重 + mse 校准 + 实拍/测试集)
板子读这条数据是 皖AMS087 (真值 沪AMS087)。"""
import os, sys, time
import numpy as np
import torch
from torch.utils.data import DataLoader

ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import quant_espdl_s3 as Q

LPRNET = Q.DEFAULT_LPRNET_DIR
sys.path.insert(0, LPRNET)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx
from esp_ppq.executor import TorchExecutor

WEIGHTS = os.path.join(ROOT, 'tools', 'out', 'finetune', 'r4_lr1e4_nofreeze.pth')
ONNX = os.path.join(ROOT, 'tools', 'out', 'lprnet_s3.onnx')
BIN = os.path.join(ROOT, 'main', 'models', 'test_input.bin')
EXPORT = os.path.join(ROOT, 'tools', 'out', 'diag_r4_mse_repro.espdl')
CROPS = r'G:\All_Project\AI_Project\BY\u4e32\u53e3\u52a9\u624b\dist\saved\images'

ALGO = sys.argv[1] if len(sys.argv) > 1 else 'mse'

for name, path in (('weights', WEIGHTS), ('onnx', ONNX), ('bin', BIN), ('校准图目录', CROPS)):
    print('%-10s %s  %s' % (name, 'OK ' if os.path.exists(path) else 'MISS', path))

torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(WEIGHTS, map_location='cpu'))
net.eval()


def show(tag, logits):
    arr = np.squeeze(np.asarray(logits, dtype=np.float32))
    if arr.shape[0] != len(CHARS):
        arr = arr.T
    lab = [int(np.argmax(arr[:, j])) for j in range(arr.shape[1])]
    blank = len(CHARS) - 1
    out, prev = [], blank
    for c in lab:
        if c != prev and c != blank:
            out.append(c)
        prev = c
    steps = ' '.join('_' if c == blank else CHARS[c] for c in lab)
    print('%-22s -> %-14s | %s' % (tag, ''.join(CHARS[i] for i in out), steps))
    return ''.join(CHARS[i] for i in out)


x = np.fromfile(BIN, dtype='<f4').reshape(24, 94, 3).transpose(2, 0, 1)
t = torch.from_numpy(np.ascontiguousarray(x)).unsqueeze(0)
show('PC float (torch r4)', net(t).numpy()[0])

samples = Q.load_calib_images(LPRNET, 128, [CROPS, os.path.join(LPRNET, 'data', 'test')])
dl = DataLoader(dataset=samples, batch_size=8, shuffle=False)
setting = QuantizationSettingFactory.espdl_setting()
setting.quantize_activation_setting.calib_algorithm = ALGO
print('量化中 (algorithm=%s) ...' % ALGO, flush=True)
t0 = time.time()
graph = espdl_quantize_onnx(
    onnx_import_file=ONNX, espdl_export_file=EXPORT,
    calib_dataloader=dl, calib_steps=len(samples),
    input_shape=[1, 3, Q.IMG_H, Q.IMG_W], target='esp32s3', num_of_bits=8,
    collate_fn=lambda b: b.to('cpu'), setting=setting, device='cpu',
    error_report=False, skip_export=True, export_config=True, verbose=0)
print('量化耗时 %.1fs' % (time.time() - t0))
ex = TorchExecutor(graph=graph, device='cpu')
oi = ex.forward(inputs=[t])[0]
if isinstance(oi, torch.Tensor):
    oi = oi.detach().cpu().numpy()
plate_i = show('PC int8 (ppq %s)' % ALGO, oi[0] if oi.ndim == 3 else oi)
print('')
print('板端读同一条数据      -> 皖AMS087   (真值 沪AMS087)')
print('结论: PC int8 %s' % ('也读错 -> 量化/校准的锅' if plate_i.startswith('\u7696') else '读对 -> 更像 esp-dl 运行端的锅'))