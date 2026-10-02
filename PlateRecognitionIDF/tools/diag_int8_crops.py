# -*- coding: utf-8 -*-
"""Isolate int8 quantization: quantize the ONNX the same way as the deployed model,
then run the SAME 94x24 crops from the board through (a) float torch and (b) int8 ppq graph.
"""
import os, sys, glob
import numpy as np
import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import quant_espdl_s3 as Q
sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
from data.load_data import CHARS

CROP_DIR = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
PATS = 'IMG_20261001_1740*.jpg'
ONNX = os.path.join(_HERE, 'out', 'lprnet_s3.onnx')
WEIGHTS = os.path.join(_HERE, 'out', 'finetune', 'r4_lr1e4_nofreeze.pth')
OUT = os.path.join(_HERE, 'out', 'diag_int8.espdl')
CALIB = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images,' + os.path.join(Q.DEFAULT_LPRNET_DIR, 'data', 'test')

from torch.utils.data import DataLoader
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx
from esp_ppq.executor import TorchExecutor
from lprnet_s3_model import build_lprnet_s3

samples = Q.load_calib_images(Q.DEFAULT_LPRNET_DIR, 96, CALIB.split(','))
dl = DataLoader(dataset=samples, batch_size=8, shuffle=False)
setting = QuantizationSettingFactory.espdl_setting()
setting.quantize_activation_setting.calib_algorithm = 'minmax'
print('quantizing ...', flush=True)
graph = espdl_quantize_onnx(
    onnx_import_file=ONNX, espdl_export_file=OUT,
    calib_dataloader=dl, calib_steps=len(samples),
    input_shape=[1, 3, Q.IMG_H, Q.IMG_W], target='esp32s3', num_of_bits=8,
    collate_fn=lambda b: b.to('cpu'), setting=setting, device='cpu',
    error_report=False, skip_export=True, export_config=True, verbose=0)
print('quantized ->', OUT, flush=True)

net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(WEIGHTS, map_location='cpu'))
net.eval()
ex = TorchExecutor(graph=graph, device='cpu')

print('')
print('%-32s %-16s %s' % ('crop', 'float(torch)', 'int8(ppq graph)'))
print('-' * 110)
for f in sorted(glob.glob(os.path.join(CROP_DIR, PATS))):
    arr = Q.preprocess(f)
    if arr is None:
        continue
    t = torch.from_numpy(arr).unsqueeze(0)
    with torch.no_grad():
        of = net(t).numpy()[0]
    oi = ex.forward(inputs=[t])[0]
    if isinstance(oi, torch.Tensor):
        oi = oi.detach().cpu().numpy()[0]
    sf = Q.greedy_decode(of, CHARS)
    si = Q.greedy_decode(oi, CHARS)
    lab_i = [int(np.argmax(np.squeeze(oi)[:, j])) for j in range(np.squeeze(oi).shape[1])]
    steps = ' '.join(CHARS[c] for c in lab_i)
    print('%-32s %-16s %s' % (os.path.basename(f), sf, si))
    print('    int8 steps: %s' % steps)