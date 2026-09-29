# -*- coding: utf-8 -*-
"""第①关: 把 LPRNetS3 导出成 esp-dl 友好的 ONNX (默认 tools/out/lprnet_s3.onnx)

做三件事:
    1. 原版 LPRNet  vs  LPRNetS3  数值等价性 (同一份权重, 多次随机输入)
    2. 导出 ONNX + onnx.checker
    3. onnxruntime 加载推理一致性 + 算子清单白名单体检

用法 (cwd 任意):
    python export_onnx_s3.py
    python export_onnx_s3.py --lprnet-dir G:\\All_Project\\AI_Project\\LPRNet_Pytorch
"""

import argparse
import collections
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

DEFAULT_LPRNET_DIR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--lprnet-dir', default=DEFAULT_LPRNET_DIR,
                   help='LPRNet_Pytorch 工程根目录 (提供 model/ 与 data/)')
    p.add_argument('--weights', default=None,
                   help='权重文件, 默认 <lprnet-dir>/weights/Final_LPRNet_model.pth')
    p.add_argument('--out', default=os.path.join(_HERE, 'out', 'lprnet_s3.onnx'))
    p.add_argument('--opset', type=int, default=13)
    p.add_argument('--repeats', type=int, default=5, help='等价性随机输入次数')
    return p.parse_args()


def main():
    args = parse_args()
    lprnet_dir = os.path.abspath(args.lprnet_dir)
    weights = args.weights or os.path.join(lprnet_dir, 'weights', 'Final_LPRNet_model.pth')
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    if lprnet_dir not in sys.path:
        sys.path.insert(0, lprnet_dir)

    import numpy as np
    import torch

    from data.load_data import CHARS
    from model.LPRNet import build_lprnet
    from lprnet_s3_model import build_lprnet_s3

    class_num = len(CHARS)
    print('=' * 72)
    print('0. 准备: 类别数 %d, 权重 %s' % (class_num, weights))
    print('=' * 72)

    net_old = build_lprnet(lpr_max_len=8, phase=False, class_num=class_num, dropout_rate=0)
    net_new = build_lprnet_s3(lpr_max_len=8, class_num=class_num, dropout_rate=0)

    state = torch.load(weights, map_location='cpu')
    net_old.load_state_dict(state)
    net_new.load_state_dict(state)          # strict=True: 键必须与原版逐键一致
    net_old.eval()
    net_new.eval()
    print('权重加载: 原版 / S3 版 均成功 (state_dict 逐键兼容)')

    # ---- 1. 等价性 ----
    print()
    print('=' * 72)
    print('1. 数值等价性 check')
    print('=' * 72)
    torch.manual_seed(0)
    max_diff = 0.0
    with torch.no_grad():
        for _ in range(args.repeats):
            x = torch.randn(1, 3, 24, 94)
            a = net_old(x)
            b = net_new(x)
            assert a.shape == b.shape, '输出形状不一致: %s vs %s' % (tuple(a.shape), tuple(b.shape))
            max_diff = max(max_diff, float((a - b).abs().max()))
    # 逐元素绝对误差受 float32 舍入影响 (归约顺序变了), 用相对误差判定
    scale = max(1e-12, float(a.abs().max()))
    rel_diff = max_diff / scale
    print('输出形状 %s | %d 次随机输入: 最大绝对差 = %.3e, 相对差 = %.3e'
          % (tuple(a.shape), args.repeats, max_diff, rel_diff))
    ok = rel_diff < 1e-5
    print('等价性:', '通过' if ok else '不通过 !!')
    if not ok:
        return 1

    # ---- 2. 导出 ONNX ----
    print()
    print('=' * 72)
    print('2. 导出 ONNX (opset %d)' % args.opset)
    print('=' * 72)
    dummy = torch.randn(1, 3, 24, 94)
    export_kwargs = dict(input_names=['input'], output_names=['output'], opset_version=args.opset)
    try:
        torch.onnx.export(net_new, dummy, args.out, dynamo=False, **export_kwargs)
    except TypeError:
        print('提示: 当前 torch 不支持 dynamo 参数, 退回默认导出器')
        torch.onnx.export(net_new, dummy, args.out, **export_kwargs)
    print('导出成功 -> %s (%.1f KB)' % (args.out, os.path.getsize(args.out) / 1024))

    import onnx

    model = onnx.load(args.out)
    onnx.checker.check_model(model)
    print('onnx.checker: 通过')
    ops = collections.Counter(n.op_type for n in model.graph.node)
    print('算子统计: %s' % dict(sorted(ops.items())))

    # ---- 3. onnxruntime 一致性 ----
    print()
    print('=' * 72)
    print('3. onnxruntime 加载 + 推理一致性')
    print('=' * 72)
    import onnxruntime as ort

    sess = ort.InferenceSession(args.out, providers=['CPUExecutionProvider'])
    x = np.random.randn(1, 3, 24, 94).astype(np.float32)
    with torch.no_grad():
        y_torch = net_new(torch.from_numpy(x)).numpy()
    y_onnx = sess.run(None, {sess.get_inputs()[0].name: x})[0]
    diff = float(np.abs(y_torch - y_onnx).max())
    print('PyTorch %s  ONNX %s  最大绝对差 = %.3e' % (y_torch.shape, y_onnx.shape, diff))
    print('ONNX 推理:', '通过' if diff < 1e-4 else '不通过 !!')

    # ---- 4. 运行端白名单体检 ----
    print()
    print('=' * 72)
    print('4. esp-dl 运行端算子白名单体检')
    print('=' * 72)
    import espdl_ops

    missing, supported = espdl_ops.check(ops)
    print('运行端已注册算子 %d 个' % len(supported))
    if missing:
        print('!! 导出图中仍含运行端未实现的算子:')
        for name, count in sorted(missing.items()):
            print('     %-16s x %d' % (name, count))
        return 2
    print('体检通过: 导出图算子全部在运行端实现范围内')

    print()
    print('产物: %s' % args.out)
    print('下一步: python quant_espdl_s3.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
