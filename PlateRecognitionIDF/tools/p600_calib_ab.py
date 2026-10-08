# -*- coding: utf-8 -*-
"""p600: 量化校准对照实验 —— 校准算法 x 校准集, 在三套验证集上打分.

要回答的两个问题 (权重一个字不改, 只动量化配置):
  1) 换校准算法 (minmax / mse / percentile / kl) 值几个点?
  2) 校准集换成"现场实拍(屏幕翻拍域)"值几个点?

只读 r4 权重与 out/lprnet_s3.onnx; 产物写到 tools/out/p600/, 绝不碰固件里的模型。
"""
import os
import sys
import time
import random

import numpy as np
import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import quant_espdl_s3 as Q

OUT = os.environ.get('P600_OUT') or os.path.join(_HERE, 'out', 'p600')
ONNX = os.environ.get('P600_ONNX') or os.path.join(_HERE, 'out', 'r4_s3.onnx')
WEIGHTS = os.path.join(_HERE, 'out', 'finetune', 'r4_lr1e4_nofreeze.pth')
FIELD = r'G:\All_Project\AI_Project\BY串口助手\dist\saved\images'
TEST = os.path.join(Q.DEFAULT_LPRNET_DIR, 'data', 'test')
HOLDOUT = os.path.join(_HERE, 'out', 'holdout')
OFFICIAL = os.path.join(Q.DEFAULT_LPRNET_DIR, 'data', 'official_val')

CALIB_N = 96          # 与文档里 "field+test96" 的口径一致

VALS = [('留出33', HOLDOUT), ('实拍', FIELD), ('皖200', OFFICIAL)]

CONFIGS = [
    ('A minmax     +实拍+test', 'minmax',     [FIELD, TEST]),
    ('B mse        +实拍+test', 'mse',        [FIELD, TEST]),
    ('C percentile +实拍+test', 'percentile', [FIELD, TEST]),
    ('D kl         +实拍+test', 'kl',         [FIELD, TEST]),
    ('E mse        +test only', 'mse',        [TEST]),
    ('F mse        +实拍 only', 'mse',        [FIELD]),
]

QT = os.environ.get('P600_QUANT') or None    # w8a8 / w8a16 / w16a16 / None
if os.environ.get('P600_ONLY_MSE'):
    CONFIGS = [c for c in CONFIGS if c[1] == 'mse']
    say_qt = QT
REPORT = os.path.join(OUT, 'results.txt')
_lines = []


def say(s):
    print(s, flush=True)
    _lines.append(s)
    try:
        with open(REPORT, 'w', encoding='utf-8') as f:
            f.write('\n'.join(_lines) + '\n')
    except Exception as e:
        print('!! 写报告失败: %s' % e, flush=True)


def gather(d):
    """返回 [(path, label)], 只保留标签字符全在 CHARS 里的样本。"""
    from data.load_data import CHARS
    out, skipped = [], 0
    for p in Q.list_imgs(d):
        labs = Q.label_of(p)
        if not labs or any(c not in CHARS for c in labs):
            skipped += 1
            continue
        out.append((p, labs))
    return out, skipped


def int8_eval(executor, items):
    hit = 0
    preds = []
    for p, gt in items:
        arr = Q.preprocess(p)
        if arr is None:
            continue
        t = torch.from_numpy(arr).unsqueeze(0)
        o = executor.forward(inputs=[t])[0]
        if isinstance(o, torch.Tensor):
            o = o.detach().cpu().numpy()[0]
        from data.load_data import CHARS
        got = Q.greedy_decode(np.squeeze(o), CHARS)
        preds.append((os.path.basename(p), gt, got))
        if got == gt:
            hit += 1
    return hit, len(preds), preds


def main():
    sys.path.insert(0, Q.DEFAULT_LPRNET_DIR)
    from data.load_data import CHARS
    from torch.utils.data import DataLoader
    from esp_ppq import QuantizationSettingFactory
    from esp_ppq.api import espdl_quantize_onnx
    from esp_ppq.executor import TorchExecutor

    os.makedirs(OUT, exist_ok=True)
    say('p600 量化校准对照实验  %s' % time.strftime('%Y-%m-%d %H:%M:%S'))
    say('ONNX   : %s' % ONNX)
    say('权重   : %s (r4)' % WEIGHTS)
    say('校准张数: %d' % CALIB_N)
    say('')

    # ---- 验证集底账(只算一次) ----
    data = {}
    for name, d in VALS:
        items, skipped = gather(d)
        data[name] = items
        cls = {}
        for _p, gt in items:
            cls[gt] = cls.get(gt, 0) + 1
        say('验证集 %-8s 可用 %4d 张 (跳过 %d 张: 标签含 CHARS 外字符) 类别: %s'
            % (name, len(items), skipped, ', '.join('%s x%d' % kv for kv in sorted(cls.items()))))
    say('')

    # ---- float 参考(只算一次) ----
    net = Q_torch_net(CHARS)
    float_acc = {}
    for name, _d in VALS:
        hit = 0
        for p, gt in data[name]:
            arr = Q.preprocess(p)
            if arr is None:
                continue
            with torch.no_grad():
                o = net(torch.from_numpy(arr).unsqueeze(0)).numpy()[0]
            if Q.greedy_decode(np.squeeze(o), CHARS) == gt:
                hit += 1
        float_acc[name] = (hit, len(data[name]))
        say('float(torch) 参考  %-8s %6.2f%%  (%d/%d)'
            % (name, 100.0 * hit / max(1, len(data[name])), hit, len(data[name])))
    say('')
    say('=' * 96)

    rows = []
    for tag, alg, dirs in CONFIGS:
        t0 = time.time()
        say('')
        say('##### %s  (alg=%s) #####' % (tag, alg))
        samples = Q.load_calib_images(Q.DEFAULT_LPRNET_DIR, CALIB_N, dirs)
        if not samples:
            say('  校准集为空, 跳过')
            continue
        dl = DataLoader(dataset=samples, batch_size=8, shuffle=False)
        st = QuantizationSettingFactory.espdl_setting()
        st.quantize_activation_setting.calib_algorithm = alg
        espdl = os.path.join(OUT, 'p600_%s_%s%s.espdl' % (tag.split()[0], alg, ('_' + QT) if QT else ''))
        graph = espdl_quantize_onnx(
            onnx_import_file=ONNX,
            espdl_export_file=espdl,
            calib_dataloader=dl,
            calib_steps=len(samples),
            input_shape=[1, 3, Q.IMG_H, Q.IMG_W],
            target='esp32s3',
            num_of_bits=8,
            collate_fn=lambda b: b.to('cpu'),
            setting=st,
            device='cpu',
            error_report=False,
            skip_export=False,
            export_config=False,
            verbose=0,
            quant_type=QT,
        )
        ex = TorchExecutor(graph=graph, device='cpu')
        row = {'tag': tag, 'alg': alg, 'size': os.path.getsize(espdl)}
        for name, _d in VALS:
            hit, tot, preds = int8_eval(ex, data[name])
            row[name] = (hit, tot)
            say('  int8 %-8s %6.2f%%  (%d/%d)' % (name, 100.0 * hit / max(1, tot), hit, tot))
            if name == '留出33':
                wrong = [w for w in preds if w[1] != w[2]]
                for bn, gt, got in wrong[:8]:
                    say('       x %-28s 真值=%-10s 读出=%s' % (bn[:28], gt, got))
        row['secs'] = time.time() - t0
        say('  用时 %.0f s, espdl %.1f KB' % (row['secs'], row['size'] / 1024.0))
        rows.append(row)

    # ---- 汇总表 ----
    say('')
    say('=' * 96)
    say('汇总 (int8 top-1; 括号内为 float 参考)')
    head = '%-26s' % '配置'
    for name, _d in VALS:
        head += '%18s' % name
    head += '%10s' % 'espdl'
    say(head)
    for r in rows:
        line = '%-26s' % r['tag']
        for name, _d in VALS:
            hit, tot = r[name]
            fh, ft = float_acc[name]
            line += '%18s' % ('%.1f%% (%.1f%%)' % (100.0 * hit / max(1, tot), 100.0 * fh / max(1, ft)))
        line += '%10s' % ('%.0fKB' % (r['size'] / 1024.0))
        say(line)
    say('')
    say('结果文件: %s' % REPORT)


def Q_torch_net(chars):
    """float 参考网络 (与 ONNX 同源的 r4 权重)"""
    import lprnet_s3_model
    net = lprnet_s3_model.build_lprnet_s3(lpr_max_len=8, class_num=len(chars), dropout_rate=0)
    sd = torch.load(WEIGHTS, map_location='cpu')
    net.load_state_dict(sd)
    net.eval()
    return net


if __name__ == '__main__':
    main()