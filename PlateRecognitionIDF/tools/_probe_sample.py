import numpy as np, torch, sys, os
sys.path.insert(0, r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools')
import quant_espdl_s3 as Q
sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

p = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\models\test_input.bin'
arr = np.fromfile(p, dtype='<f4')
print('原始 shape', arr.shape, '范围 [%.4f, %.4f]' % (arr.min(), arr.max()))
hwc = arr.reshape(24, 94, 3)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\finetune\r4_lr1e4_nofreeze.pth', map_location='cpu'))
net.eval()
t = torch.from_numpy(np.ascontiguousarray(hwc.transpose(2,0,1))[None].astype(np.float32))
with torch.no_grad():
    o = net(t).numpy()[0]
print('float(torch, r4)  ->', Q.greedy_decode(o, CHARS))
# int8 模拟: 用板端同样的 LUT 量化 (q = clamp(round((v-127.5)/128*128)) = clamp(round(v-127.5)))
qimg = np.clip(np.floor(hwc * 128.0 + 0.5), -128, 127).astype(np.int8)
print('按板端 LUT 量化后 -> 与原始 float 的最大差 %.4f (应≈0)' % np.abs(qimg.astype(np.float32)/128.0 - hwc).max())
import cv2
vis = ((hwc * 128.0 + 127.5)).astype(np.uint8)
cv2.imwrite(r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\test_input_vis.png', cv2.resize(vis, (94*6, 24*6), interpolation=cv2.INTER_NEAREST))
print('已渲染 -> tools/out/test_input_vis.png')