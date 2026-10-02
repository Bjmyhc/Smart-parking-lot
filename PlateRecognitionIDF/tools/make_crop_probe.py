# -*- coding: utf-8 -*-
"""P5.50 自检探针: 把串口发回来的 94x24 模型输入块还原成板端能直接吃的 float32 裸张量。

为什么能还原 (逐位一致):
  板端发图 (main.cpp:2377-2388): g_crop_u8 = R,G,B, 每通道 v = q + 127 (q 就是模型输入的 int8)
  => PC 用 cv2.imdecode 得到的 BGR 数组 == 模型真正吃到的通道顺序 (训练链路也是 cv2 BGR)
  板端模型输入: q = clamp(floor(v - 127.5 + 0.5))  (exponent=-7 => scale=1/128)
  探针 float:   f = (v - 127.5) / 128
  assign() 内部 quantize<int8>(f) = round(f * 128) = v - 127 = q  -> 与板端喂给模型的值完全一致
  唯一误差来源: JPEG 有损压缩 (板端发图走的是 JPEG)
"""
import os
import numpy as np
import cv2

IMG_DIR = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
SRC = os.path.join(IMG_DIR, 'IMG_20261001_174038_397174.jpg')
ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
OUT = os.path.join(ROOT, 'main', 'models', 'probe_crop.bin')
PNG = os.path.join(ROOT, 'tools', 'out', 'probe_crop_ref.png')

img = cv2.imdecode(np.fromfile(SRC, np.uint8), cv2.IMREAD_COLOR)
if img is None:
    raise SystemExit('decode failed: ' + SRC)
print('src  %s  shape=%s' % (os.path.basename(SRC), img.shape))
if img.shape[0] != 24 or img.shape[1] != 94:
    img = cv2.resize(img, (94, 24), interpolation=cv2.INTER_AREA)
    print('resized -> %s' % (img.shape,))

x = (img.astype(np.float32) - 127.5) * (1.0 / 128.0)
blob = np.ascontiguousarray(x, dtype='<f4')
with open(OUT, 'wb') as fh:
    fh.write(blob.tobytes())
print('write %s  %d bytes (expect %d)' % (OUT, blob.nbytes, 94 * 24 * 3 * 4))

q = np.clip(np.floor(x * 128.0 + 0.5), -128, 127).astype(np.int8)
print('int8  min=%d max=%d mean=%.1f' % (q.min(), q.max(), q.mean()))
cv2.imwrite(PNG, cv2.resize(img, (94 * 8, 24 * 8), interpolation=cv2.INTER_NEAREST))
print('png  %s' % PNG)