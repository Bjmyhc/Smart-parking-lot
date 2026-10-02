# -*- coding: utf-8 -*-
"""Test which quantization recipe reproduces the board's collapse (jingQYue)."""
import os, sys, glob
import numpy as np
import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import quant_espdl_s3 as Q
sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
from data.load_data import CHARS
from torch.utils.data import DataLoader
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx
from esp_ppq.executor import TorchExecutor
from lprnet_s3_model import build_lprnet_s3

CROP_DIR = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
PATS = 'IMG_20261001_1740*.jpg'
ONNX = os.path.join(_HERE, 'out', 'lprnet_s3.onnx')
WEIGHTS = os.path.join(_HERE, 'out', 'finetune', 'r4_lr1e4_nofreeze.pth')
FIELD = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
TEST = os.path.join(Q.DEFAULT_LPRNET_DIR, 'data', 'test')

crops = sorted(glob.glob(os.path.join(CROP_DIR, PATS)))
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(WEIGHTS, map_location='cpu'))
net.eval()
print('== float(torch) 参考 ==')
for f in crops:
    arr = Q.preprocess(f)
    t = torch.from_numpy(arr).unsqueeze(0)
    with torch.no_grad():
        print('   %-30s %s' % (os.path.basename(f), Q.greedy_decode(net(t).numpy()[0], CHARS)))

CONFIGS = [
    ('minmax + field+test96', 'minmax', FIELD + ',' + TEST),
    ('minmax + test only',    'minmax', TEST),
    ('kl     + field+test96', 'kl',     FIELD + ',' + TEST),
    ('minmax + field only',   'minmax', FIELD),
]
for name, alg, dirs in CONFIGS:
    try:
        samples = Q.load_calib_images(Q.DEFAULT_LPRNET_DIR, 96, dirs.split(','))
        dl = DataLoader(dataset=samples, batch_size=8, shuffle=False)
        st = QuantizationSettingFactory.espdl_setting()
        st.quantize_activation_setting.calib_algorithm = alg
        g = espdl_quantize_onnx(onnx_import_file=ONNX, espdl_export_file=os.path.join(_HERE,'out','diag_%s.espdl' % alg),
            calib_dataloader=dl, calib_steps=len(samples), input_shape=[1,3,Q.IMG_H,Q.IMG_W],
            target='esp32s3', num_of_bits=8, collate_fn=lambda b: b.to('cpu'), setting=st,
            device='cpu', error_report=False, skip_export=False, export_config=False, verbose=0)
        ex = TorchExecutor(graph=g, device='cpu')
        print('')
        print('===== %s =====' % name)
        for f in crops:
            arr = Q.preprocess(f)
            t = torch.from_numpy(arr).unsqueeze(0)
            oi = ex.forward(inputs=[t])[0]
            if isinstance(oi, torch.Tensor):
                oi = oi.detach().cpu().numpy()[0]
            a = np.squeeze(oi)
            lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
            print('   %-30s %s' % (os.path.basename(f), ''.join(CHARS[c] for c in lab)))
    except Exception as e:
        print('   !! %s 失败: %s' % (name, e))