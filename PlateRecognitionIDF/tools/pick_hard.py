# -*- coding: utf-8 -*-
"""模式 2 素材分拣: 把串口采下来的 94x24 模型输入块, 按"当前模型读对/读错"自动分成两组.

为什么用这个, 而不是拿 prep_collect.py 去裁预览图
------------------------------------------------
短按 BOOT 切到"模式 2"时, 串口发出来的就是模型真正吃到的那张 94x24 图
(固件 main.cpp: model_input 里的 int8 经 g_inv_lut 反量化回 uint8 再编码), 于是:

  * 不用再"找车牌" -> prep_collect.py 那套几何裁剪的坑直接绕开;
  * 素材与推理输入逐像素一致 (只剩下最后一道 JPEG q90 的损耗) -> 训的和跑的同一个分布.

素材按价值分三档 (上次微调就是死在第一档上):
  1) 裁得对 + 读得错  -> 黄金难样本, 微调就靠它
  2) 裁得对 + 读得对  -> 正常样本, 占 70~80%
  3) 裁错了          -> 废图, 不是"难图". 喂进去只会教坏模型.

所以本脚本不急着训, 先把"读错的那批"放大成一张拼图, 肉眼确认裁剪到底对不对.

用法
----
  python -X utf8 pick_hard.py <源目录> <真值标签> [--out out/train2] [--dedup-thresh 2.5]

例
--
  python -X utf8 pick_hard.py "G:/All_Project/AI_Project/BY串口助手/dist/saved/images/京Q-06666" 京Q06666

产物
----
  out/train2/<标签>/<标签>_NNN.jpg            全部保留帧 (文件名自带真值, 可直接 --data-dirs 喂 finetune_s3.py)
  out/hard/<标签>_wrong_montage.png           读错的那批, 放大拼图 (每行左上角是序号)
  out/hard/<标签>/<标签>_errNNN=<预测>.jpg     读错的单张
  out/hard/<标签>_report.txt                  逐张的"真值 -> 预测"清单

注意
----
  * 源图是原字节复制过去的, 不再重编码 -> 不给你多加一代 JPEG 损耗.
  * 连续帧 (200 ms 一张) 几乎一样, 默认按"和上一张保留帧的平均差"去重,
    否则训练/验证会被这些双胞胎帧灌水, 验证正确率虚高. --dedup-thresh 0 可关掉.
"""

import argparse
import glob
import os
import shutil
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

DEFAULT_LPRNET_DIR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
IMG_W, IMG_H = 94, 24


def load_bgr(path):
    import cv2
    import numpy as np
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def to_tensor(img, mean=127.5, std=128.0):
    import numpy as np
    im = img.astype(np.float32)
    im = (im - mean) / std
    return im.transpose(2, 0, 1)


def ctc_decode(seq, chars):
    """贪心 + 去重 + 去 blank, 与 finetune_s3.py 的 evaluate() 逐行一致"""
    blank = len(chars) - 1
    res, prev = [], -1
    for c in seq:
        c = int(c)
        if c != prev and c != blank:
            res.append(chars[c])
        prev = c
    return ''.join(res)


def quality_metrics(img):
    """一帧的成像体检: (亮度均值, 对比度, 锐度, 过曝%, 死黑%).

    锐度 = 平均 |Laplacian|, 灰度系数与固件 crop_luma 一致 ((77R + 150G + 29B) >> 8).
    ⚠ 口径差别: 固件日志里那个"锐度"算在【光度归一化之前】的裁剪块上, 而模式 2 存下来的
      已经是【归一化之后】的模型输入。归一化只拉不压(倍数 >= 1), 所以这里的数会比日志里
      偏大。帧与帧之间互相比没问题, 别拿它跟日志里的绝对值硬比。
    """
    import cv2
    import numpy as np
    luma = ((77 * img[:, :, 2].astype(np.int32) + 150 * img[:, :, 1] + 29 * img[:, :, 0]) >> 8)
    luma = luma.astype(np.uint8)
    lap = np.abs(cv2.Laplacian(luma.astype(np.float32), cv2.CV_32F))
    mx = img.max(axis=2).astype(np.int32)
    n = float(luma.size)
    # 省字那一格: 94 宽摊 7 个字, 一个字约 13~15 px, 所以左起 16 列基本就是省份字。
    # 为什么要单独算: 右边那些又大又白的字母数字边很硬, 会把全局锐度抬上去, 把"省字糊了"盖掉。
    lap_prov = lap[:, 1:17]
    std_prov = luma[:, 1:17].astype(np.float32).std()
    return (float(luma.mean()), float(luma.std()), float(lap.mean()),
            100.0 * float((mx >= 250).sum()) / n, 100.0 * float((mx <= 5).sum()) / n,
            float(lap_prov.mean()), float(std_prov))


def find_images(root):
    out = []
    for ext in ('*.jpg', '*.jpeg', '*.png', '*.bmp'):
        out += glob.glob(os.path.join(root, '**', ext), recursive=True)
    return sorted(out)


def montage(tiles, scale, path, indexes):
    """把若干 94x24 小块放大拼成一张竖条图, 左上角标序号 (ASCII 数字, cv2 写不了中文)"""
    import cv2
    import numpy as np
    if not tiles:
        return False
    rows = []
    for t, idx in zip(tiles, indexes):
        big = cv2.resize(t, (t.shape[1] * scale, t.shape[0] * scale),
                         interpolation=cv2.INTER_NEAREST)
        cv2.putText(big, str(idx), (4, 17), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (0, 255, 255), 1, cv2.LINE_AA)
        rows.append(big)
        rows.append(np.full((4, big.shape[1], 3), 255, np.uint8))
    canvas = np.vstack(rows[:-1])
    cv2.imencode('.png', canvas)[1].tofile(path)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='串口助手存下来的目录 (会递归找图)')
    ap.add_argument('label', help='这批复拍的真值车牌, 例如 京Q06666')
    ap.add_argument('--lprnet-dir', default=DEFAULT_LPRNET_DIR)
    ap.add_argument('--weights', default=None)
    ap.add_argument('--out', default=os.path.join(_HERE, 'out', 'train2'))
    ap.add_argument('--dedup-thresh', type=float, default=2.5,
                    help='与上一张保留帧的平均绝对差小于此值 -> 判为重复帧丢掉 (0 = 不去重)')
    ap.add_argument('--montage-scale', type=int, default=6)
    a = ap.parse_args()

    label = a.label.replace('-', '').replace('_', '')
    lprnet_dir = os.path.abspath(a.lprnet_dir)
    if lprnet_dir not in sys.path:
        sys.path.insert(0, lprnet_dir)

    import numpy as np
    import torch
    import cv2
    from data.load_data import CHARS, CHARS_DICT
    from model.LPRNet import build_lprnet

    if not all(c in CHARS_DICT for c in label):
        print('标签里有训练集认不得的字符:', label)
        print('  (CHARS 里没有 -> finetune_s3.py 会把它整张丢掉, 先确认牌子读对没有)')
        return 2

    files = find_images(a.src)
    print('源目录: %s' % a.src)
    print('找到 %d 张图' % len(files))
    if not files:
        return 1

    weights = a.weights or os.path.join(lprnet_dir, 'weights', 'Final_LPRNet_model.pth')
    net = build_lprnet(lpr_max_len=8, phase=True, class_num=len(CHARS), dropout_rate=0.5)
    net.load_state_dict(torch.load(weights, map_location='cpu'))
    net.eval()
    print('权重: %s' % weights)

    ok_dir = os.path.join(a.out, label)
    hard_dir = os.path.join(_HERE, 'out', 'hard')
    hard_sub = os.path.join(hard_dir, label)
    os.makedirs(ok_dir, exist_ok=True)
    os.makedirs(hard_sub, exist_ok=True)

    kept, dropped, bad_size = [], [], []
    prev = None
    n_ok = n_bad = 0
    conf = Counter()
    report = []
    wrong_tiles, wrong_idx = [], []

    for p in files:
        img = load_bgr(p)
        if img is None:
            report.append('%s  读不出来, 跳过' % os.path.basename(p))
            continue
        if img.shape[1] != IMG_W or img.shape[0] != IMG_H:
            bad_size.append((os.path.basename(p), img.shape[1], img.shape[0]))
            img = cv2.resize(img, (IMG_W, IMG_H))

        if a.dedup_thresh > 0 and prev is not None:
            d = float(np.mean(np.abs(img.astype(np.int16) - prev.astype(np.int16))))
            if d < a.dedup_thresh:
                dropped.append((os.path.basename(p), d))
                continue
        prev = img

        x = torch.from_numpy(np.ascontiguousarray(to_tensor(img)))[None]
        with torch.no_grad():
            logits = net(x)[0].numpy()
        pred = ctc_decode(logits.argmax(0), CHARS)

        kept.append((p, img, pred, quality_metrics(img)))
        if pred == label:
            n_ok += 1
        else:
            n_bad += 1
            conf['%s -> %s' % (label, pred)] += 1

    print('')
    print('去重丢掉 %d 张 (阈值 %.1f)' % (len(dropped), a.dedup_thresh))
    if bad_size:
        print('!! 有 %d 张不是 94x24 —— 那说明源目录里混进了预览图, 微调前必须清掉:' % len(bad_size))
        for name, w, h in bad_size[:10]:
            print('     %s  %dx%d' % (name, w, h))
    print('保留 %d 张: 读对 %d, 读错 %d (当前模型准确率 %.1f%%)'
          % (len(kept), n_ok, n_bad, 100.0 * n_ok / max(1, len(kept))))

    for i, (p, img, pred, _q) in enumerate(kept, 1):
        name = '%s_%03d%s' % (label, i, os.path.splitext(p)[1].lower())
        shutil.copyfile(p, os.path.join(ok_dir, name))
        if pred != label:
            err = '%s_err%03d=%s%s' % (label, i, pred, os.path.splitext(p)[1].lower())
            shutil.copyfile(p, os.path.join(hard_sub, err))
            report.append('#%d %s  ->  %s' % (i, os.path.basename(p), pred))
        else:
            report.append('#%d %s  ->  %s  (对)' % (i, os.path.basename(p), pred))

    if conf:
        print('')
        print('错法排行:')
        for k, v in conf.most_common(12):
            print('   %-24s %d 张' % (k, v))

    if kept:
        print('')
        print('成像体检: mean=亮度均值 std=对比度 sharp=平均|Laplacian| over/dark=过曝/死黑占比')
        print('  %-4s %-10s %-4s %7s %7s %7s %6s %6s | %8s %8s'
              % ('#', 'pred', 'ok', 'mean', 'std', 'sharp', 'over', 'dark', 'shProv', 'stdProv'))
        for i, (_p, _img, pred, q) in enumerate(kept, 1):
            print('  %-4d %-10s %-4s %7.1f %7.1f %7.2f %6.1f %6.1f | %8.2f %8.1f'
                  % (i, pred, ('Y' if pred == label else 'N'), q[0], q[1], q[2], q[3], q[4], q[5], q[6]))
        g_ok = [q for (_p, _i, pr, q) in kept if pr == label]
        g_bad = [q for (_p, _i, pr, q) in kept if pr != label]
        if g_bad and g_ok:
            def _avg(g, k):
                return sum(x[k] for x in g) / float(len(g))
            print('  读对组 n=%d: 全局锐度 %.2f | 省区锐度 %.2f | 省区对比度 %.1f'
                  % (len(g_ok), _avg(g_ok, 2), _avg(g_ok, 5), _avg(g_ok, 6)))
            print('  读错组 n=%d: 全局锐度 %.2f | 省区锐度 %.2f | 省区对比度 %.1f'
                  % (len(g_bad), _avg(g_bad, 2), _avg(g_bad, 5), _avg(g_bad, 6)))
            print('  (两组差得越少, 越说明"不是看不清, 是模型本身偏了")')
        wrong_tiles = [img for _, img, pred, _ in kept if pred != label]
        wrong_idx = [i for i, (_, _, pred, _) in enumerate(kept, 1) if pred != label]
        mpath = os.path.join(hard_dir, '%s_wrong_montage.png' % label)
        if montage(wrong_tiles, a.montage_scale, mpath, wrong_idx):
            print('')
            print('读错的 %d 张放大拼图 (先看这个! 确认裁剪对不对):' % len(wrong_tiles))
            print('   %s' % mpath)
        all_tiles = [img for _, img, _, _ in kept]
        all_idx = list(range(1, len(kept) + 1))
        apath = os.path.join(hard_dir, '%s_all_montage.png' % label)
        if montage(all_tiles, a.montage_scale, apath, all_idx):
            print('全部 %d 张拼图:' % len(all_tiles))
            print('   %s' % apath)

    rpath = os.path.join(hard_dir, '%s_report.txt' % label)
    with open(rpath, 'w', encoding='utf-8') as f:
        f.write('源目录: %s\n真值: %s\n保留 %d 张 / 读对 %d / 读错 %d\n' % (a.src, label, len(kept), n_ok, n_bad))
        f.write('去重丢掉 %d 张 (阈值 %.1f)\n\n' % (len(dropped), a.dedup_thresh))
        f.write('\n'.join(report))
    print('清单: %s' % rpath)
    print('')
    print('下一步: 确认上面那张拼图里"裁得对"的比例够高, 再拿 %s 当 --data-dirs 跑 finetune_s3.py' % a.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())