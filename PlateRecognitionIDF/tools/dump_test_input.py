# -*- coding: utf-8 -*-
"""从 LPRNet_Pytorch/data/test 挑一张图, 按训练时的预处理压成 float32 裸张量,
写成 main/models/test_input.bin, 供板端加载验证。

布局 (重要)
    默认输出 HWC (逐像素交织), 与 esp-dl 内部 NHWC 张量一致:
        dl_image_preprocessor.cpp: shape[1]=H, shape[2]=W, shape[3]=C
    板端 main.cpp 用 shape={1,24,94,3} 装载本文件。
    加 --chw 可以输出 CHW, 那只用于和 PC 端 torch 张量直接比对, 别烧进板子。

用法:
    python dump_test_input.py --name 沪AMS087
    python dump_test_input.py <图片路径>
"""

import argparse
import os
import sys

import cv2
import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LPRNET_DIR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
IMG_W, IMG_H = 94, 24


def preprocess(img):
    """与训练 / predict.py 一致: resize 94x24, (x-127.5)*0.0078125。返回 HWC"""
    img = cv2.resize(img, (IMG_W, IMG_H))
    img = img.astype('float32')
    img -= 127.5
    img *= 0.0078125
    return img


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('image', nargs='?', help='图片路径 (省略则用 --name 在 data/test 里找)')
    parser.add_argument('--lprnet-dir', default=DEFAULT_LPRNET_DIR)
    parser.add_argument('--data-dir', default=None, help='默认 <lprnet-dir>/data/test')
    parser.add_argument('--name', default='沪AMS087', help='data/test 里的图片名 (可带扩展名)')
    parser.add_argument('--out', default=os.path.join(_HERE, '..', 'main', 'models', 'test_input.bin'))
    parser.add_argument('--chw', action='store_true', help='输出 CHW (仅供 PC 比对, 不要烧板)')
    args = parser.parse_args()

    data_dir = args.data_dir or os.path.join(args.lprnet_dir, 'data', 'test')
    if args.image:
        path = args.image
    else:
        name = args.name if os.path.splitext(args.name)[1] else args.name + '.jpg'
        path = os.path.join(data_dir, name)
    if not os.path.isfile(path):
        raise SystemExit('读不到图片: ' + path)

    label = os.path.splitext(os.path.basename(path))[0].split('-')[0].split('_')[0]

    img = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit('解码失败: ' + path)

    arr = preprocess(img)
    if args.chw:
        arr = np.transpose(arr, (2, 0, 1))          # HWC(BGR) -> CHW
    arr = np.ascontiguousarray(arr, dtype='<f4')

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    arr.tofile(args.out)

    print('源图   :', path)
    print('标签   :', label)
    print('布局   :', 'CHW (仅供 PC 比对)' if args.chw else 'HWC/NHWC (板端用)')
    print('张量   : shape={} dtype=float32 字节={}'.format(arr.shape, arr.size * 4))
    print('输出   :', os.path.abspath(args.out))
    print('数值域 : [{:.4f}, {:.4f}]'.format(arr.min(), arr.max()))
    print('板端应输出: >>> RESULT: ' + label)


if __name__ == '__main__':
    main()
