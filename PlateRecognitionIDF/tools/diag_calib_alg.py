# -*- coding: utf-8 -*-
"""Does the calibration algorithm explain the board's collapse?"""
import os, sys, glob
import numpy as np
import torch
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import quant_espdl_s3 as Q
sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
from data.load_data import CHARS
from torch.utils.data import DataLoader
from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx
from esp_ppq.executor import TorchExecutor

CROP_DIR = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
ONNX = os.path.join(Q.__file__).replace('quant_espdl_s3.py', 'out') + r'\lprnet_s3.onnx'
FIELD = CROP_DIR
TEST = os.path.join(Q.DEFAULT_LPRNET_DIR, 'data', 'test')
crops = sorted(glob.glob(os.path.join(CROP_DIR, 'IMG_20261001_1740*.jpg')))

BOARD = {'174038':'京Q粤','174039':'京粤Q粤浙粤','174040_1':'粤京粤','174040_9':'京Q粤',
         '174041':'京粤','174042':'京粤粤粤','174043':'京Q粤粤','174044':'浙京Q粤粤粤',
         '174045':'粤京粤粤','174046':'粤浙粤浙粤浙粤浙粤'}

for alg in ('minmax', 'mse', 'kl'):
    for calib_tag, calib in (('data/test', TEST), ('实拍+test', FIELD + ',' + TEST)):
        samples = Q.load_calib_images(Q.DEFAULT_LPRNET_DIR, 96, calib.split(','))
        dl = DataLoader(dataset=samples, batch_size=8, shuffle=False)
        st = QuantizationSettingFactory.espdl_setting()
        st.quantize_activation_setting.calib_algorithm = alg
        g = espdl_quantize_onnx(onnx_import_file=ONNX,
            espdl_export_file=os.path.join(os.path.dirname(ONNX), 'diag_%s_tmp.espdl' % alg),
            calib_dataloader=dl, calib_steps=len(samples), input_shape=[1,3,Q.IMG_H,Q.IMG_W],
            target='esp32s3', num_of_bits=8, collate_fn=lambda b: b.to('cpu'), setting=st,
            device='cpu', error_report=False, skip_export=False, export_config=False, verbose=0)
        ex = TorchExecutor(graph=g, device='cpu')
        print('')
        print('##### alg=%-6s calib=%-12s #####' % (alg, calib_tag))
        for f in crops:
            arr = Q.preprocess(f)
            t = torch.from_numpy(arr).unsqueeze(0)
            o = ex.forward(inputs=[t])[0]
            if isinstance(o, torch.Tensor): o = o.detach().cpu().numpy()[0]
            a = np.squeeze(o); T = a.shape[1]
            lab = [int(np.argmax(a[:, j])) for j in range(T)]
            txt, prev, res = [], 67, []
            for c in lab:
                if c != prev and c != 67: res.append(c)
                prev = c
            txt = ''.join(CHARS[c] for c in res)
            bn = os.path.basename(f); key = bn[13:19]
            if key == '174040': key += '_1' if bn[20] == '1' else '_9'
            print('   %-24s 板端=%-14s PC=%-16s' % (os.path.basename(f)[4:22], BOARD[key], txt))