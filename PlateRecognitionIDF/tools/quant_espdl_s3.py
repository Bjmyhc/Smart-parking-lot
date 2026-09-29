# -*- coding: utf-8 -*-
"""第②关: esp-ppq int8 量化, 产出 lprnet_s3.espdl

输入: tools/out/lprnet_s3.onnx      (第①关产物, 已消除运行端不认识的算子)
输出: tools/out/lprnet_s3.espdl

校准数据用 <lprnet-dir>/data/test 里的真实车牌图。

用法:
    python quant_espdl_s3.py
    python quant_espdl_s3.py --eval-n 200          # 量化后顺便测一下 int8 精度
"""

import argparse
import os
import random
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

DEFAULT_LPRNET_DIR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
IMG_W, IMG_H = 94, 24


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--lprnet-dir', default=DEFAULT_LPRNET_DIR)
    p.add_argument('--onnx', default=os.path.join(_HERE, 'out', 'lprnet_s3.onnx'))
    p.add_argument('--out', default=os.path.join(_HERE, 'out', 'lprnet_s3.espdl'))
    p.add_argument('--calib-n', type=int, default=128)
    p.add_argument('--calib-algorithm', default='minmax',
                   help="espdl_setting 默认 kl; 本模型用 kl 会把归一化分母裁掉, 精度暴跌")
    p.add_argument('--eval-n', type=int, default=0, help='>0 时用验证集测 int8 精度')
    p.add_argument('--eval-dir', default=None)
    p.add_argument('--weights', default=None)
    return p.parse_args()


def preprocess(path):
    """与训练 / predict.py 完全一致: BGR, resize 94x24, (x-127.5)*0.0078125, HWC->CHW"""
    import cv2
    import numpy as np

    img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return None
    img = cv2.resize(img, (IMG_W, IMG_H))
    img = img.astype(np.float32)
    img = (img - 127.5) * 0.0078125
    return img.transpose(2, 0, 1)


def label_of(path):
    name = os.path.splitext(os.path.basename(path))[0]
    return name.split('-')[0].split('_')[0]


def greedy_decode(logits, chars):
    """与 predict.py 的 greedy_decode 严格一致。logits: [C, T]"""
    import numpy as np

    arr = np.asarray(logits, dtype=np.float32)
    arr = np.squeeze(arr)
    if arr.ndim != 2:
        raise ValueError('无法解释的模型输出形状: %s' % (np.asarray(logits).shape,))
    if arr.shape[0] != len(chars):
        arr = arr.T
    blank = len(chars) - 1

    preb_label = [int(np.argmax(arr[:, j], axis=0)) for j in range(arr.shape[1])]
    result = []
    pre_c = preb_label[0]
    if pre_c != blank:
        result.append(pre_c)
    for c in preb_label:
        if (pre_c == c) or (c == blank):
            if c == blank:
                pre_c = c
            continue
        result.append(c)
        pre_c = c
    return ''.join(chars[i] for i in result)


def load_calib_images(lprnet_dir, count):
    import torch

    calib_dir = os.path.join(lprnet_dir, 'data', 'test')
    names = [f for f in os.listdir(calib_dir) if f.lower().endswith(('.jpg', '.png'))]
    random.seed(0)
    random.shuffle(names)
    names = names[:count]

    samples = []
    for name in names:
        arr = preprocess(os.path.join(calib_dir, name))
        if arr is not None:
            samples.append(torch.from_numpy(arr))
    print('校准图: 载入 %d 张 <- %s' % (len(samples), calib_dir))
    return samples


def eval_accuracy(graph, lprnet_dir, eval_dir, eval_n, chars, weights):
    """float(torch) 与 int8(ppq graph) 在同一批图上对比 top-1 准确率"""
    import numpy as np
    import torch
    from esp_ppq.executor import TorchExecutor
    from lprnet_s3_model import build_lprnet_s3

    names = [f for f in os.listdir(eval_dir) if f.lower().endswith(('.jpg', '.png'))]
    random.seed(1234)
    random.shuffle(names)
    names = names[:eval_n]

    net = build_lprnet_s3(lpr_max_len=8, class_num=len(chars), dropout_rate=0)
    net.load_state_dict(torch.load(weights, map_location='cpu'))
    net.eval()

    executor = TorchExecutor(graph=graph, device='cpu')

    total = 0
    hit_float = 0
    hit_int8 = 0
    for name in names:
        arr = preprocess(os.path.join(eval_dir, name))
        if arr is None:
            continue
        gt = label_of(os.path.join(eval_dir, name))
        if any(c not in chars for c in gt):
            continue
        total += 1
        tensor = torch.from_numpy(arr).unsqueeze(0)
        with torch.no_grad():
            out_float = net(tensor).numpy()[0]
        out_int8 = executor.forward(inputs=[tensor])[0]
        if isinstance(out_int8, torch.Tensor):
            out_int8 = out_int8.detach().cpu().numpy()[0]
        if greedy_decode(out_float, chars) == gt:
            hit_float += 1
        if greedy_decode(out_int8, chars) == gt:
            hit_int8 += 1

    print('')
    print('=' * 72)
    print('量化精度体检 (%s, %d 张)' % (eval_dir, total))
    print('=' * 72)
    if total:
        print('float  top-1: %.2f%%  (%d/%d)' % (100.0 * hit_float / total, hit_float, total))
        print('int8   top-1: %.2f%%  (%d/%d)' % (100.0 * hit_int8 / total, hit_int8, total))
    else:
        print('没有可用样本')
    return total


def main():
    args = parse_args()
    lprnet_dir = os.path.abspath(args.lprnet_dir)
    weights = args.weights or os.path.join(lprnet_dir, 'weights', 'Final_LPRNet_model.pth')
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if not os.path.exists(args.onnx):
        print('找不到 %s, 请先跑 export_onnx_s3.py' % args.onnx)
        return 1

    import torch
    from torch.utils.data import DataLoader
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.api import espdl_quantize_onnx

    samples = load_calib_images(lprnet_dir, args.calib_n)
    if not samples:
        print('校准图载入失败')
        return 1

    calib_dataloader = DataLoader(dataset=samples, batch_size=8, shuffle=False)

    def collate_fn(batch):
        return batch.to('cpu')

    quant_setting = QuantizationSettingFactory.espdl_setting()
    quant_setting.quantize_activation_setting.calib_algorithm = args.calib_algorithm

    print('=' * 72)
    print('开始量化: target=esp32s3 bits=8 calib_steps=%d algorithm=%s'
          % (len(samples), args.calib_algorithm))
    print('=' * 72)

    graph = espdl_quantize_onnx(
        onnx_import_file=args.onnx,
        espdl_export_file=args.out,
        calib_dataloader=calib_dataloader,
        calib_steps=len(samples),
        input_shape=[1, 3, IMG_H, IMG_W],
        target='esp32s3',
        num_of_bits=8,
        collate_fn=collate_fn,
        setting=quant_setting,
        device='cpu',
        error_report=True,
        skip_export=False,
        export_config=True,
        verbose=1,
    )

    if not os.path.exists(args.out):
        print('未生成 espdl 文件')
        return 1
    print('')
    print('量化完成 -> %s (%.1f KB)' % (args.out, os.path.getsize(args.out) / 1024))

    if args.eval_n > 0:
        sys.path.insert(0, lprnet_dir)
        from data.load_data import CHARS

        eval_dir = args.eval_dir or os.path.join(lprnet_dir, 'data', 'official_val')
        eval_accuracy(graph, lprnet_dir, eval_dir, args.eval_n, CHARS, weights)

    print('')
    print('下一步: python inspect_espdl.py %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
