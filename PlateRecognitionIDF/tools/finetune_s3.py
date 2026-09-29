# -*- coding: utf-8 -*-
"""用采到的实拍素材微调 LPRNet, 产出可直接部署的权重.

为什么必须做 (P5.17, 2026-09-29)
--------------------------------
离线复核 (同一批 97 张实拍图, 裁剪框只动缩放) 得到:

    横向缩放 sx   0.90   0.95   1.00   1.05   1.10   1.15
    正确率        48.5%  32.0%  61.9%  77.3%  35.1%   7.2%

sx 差 5% 就能从 78% 掉到 35% —— 这说明当前模型把"车牌在张量里占多宽"背死了.
端侧的框是设备自己算的, 和离线复现的框不可能逐像素一致, 所以靠"调框参数去凑"
这条路很脆; 真正吃得住现场的做法是让模型对几何/亮度不敏感, 也就是做增强微调.

做法
----
  * 训练集 = 实拍裁剪图 + CCPD 训练集抽样 (replay, 防止把通用车牌能力忘掉)
  * 几何增强: 横/纵缩放 0.88~1.14、平移 ±10%、旋转 ±3 度
  * 光度增强: 亮度/对比度抖动 (屏幕翻拍普遍偏亮)
  * 小学习率从 weights/Final_LPRNet_model.pth 接着训
  * 每轮在 (a) 留出的实拍图 (b) CCPD 验证集 上分别测, 取两边都不掉的权重

用法
----
  python finetune_s3.py --epochs 15
产物
----
  tools/out/finetune/finetuned_s3.pth   (直接喂给 export_onnx_s3.py --weights)
"""

import argparse
import os
import random
import sys
import time
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

DEFAULT_LPRNET_DIR = r'G:\All_Project\AI_Project\LPRNet_Pytorch'
IMG_W, IMG_H = 94, 24


def label_of(path):
    name = os.path.splitext(os.path.basename(path))[0]
    return name.split('-')[0].split('_')[0]


def load_bgr(path):
    import cv2
    import numpy as np
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def to_tensor(img, mean=127.5, std=128.0):
    import numpy as np
    im = img.astype(np.float32)
    im = (im - mean) / std
    return im.transpose(2, 0, 1)


def augment(img, rng):
    """几何 + 光度增强. img: HWC uint8 BGR (94x24)"""
    import cv2
    import numpy as np
    h, w = img.shape[:2]
    if rng.random() < 0.95:
        sx = rng.uniform(0.88, 1.14)
        sy = rng.uniform(0.88, 1.14)
        ang = rng.uniform(-3.0, 3.0)
        cx, cy = (w - 1) / 2.0, (h - 1) / 2.0
        a = np.deg2rad(ang)
        ca, sa = np.cos(a), np.sin(a)
        tx = rng.uniform(-0.10, 0.10) * w
        ty = rng.uniform(-0.06, 0.06) * h
        M = np.float32([
            [sx * ca, -sx * sa, cx * (1 - sx * ca) + cy * sx * sa + tx],
            [sy * sa,  sy * ca, cy * (1 - sy * ca) - cx * sy * sa + ty],
        ])
        img = cv2.warpAffine(img, M, (w, h), flags=cv2.INTER_LINEAR,
                             borderMode=cv2.BORDER_REPLICATE)
    if rng.random() < 0.80:
        f = img.astype(np.float32)
        f = f * rng.uniform(0.80, 1.25) + rng.uniform(-25.0, 25.0)
        img = np.clip(f, 0, 255).astype(np.uint8)
    return img


def collect(dirs):
    import glob
    out = []
    for d in dirs:
        for ext in ('*.jpg', '*.jpeg', '*.png'):
            out += sorted(glob.glob(os.path.join(d, ext)))
    return out


def split_holdout(paths, every=5):
    """每 every 张留一张做验证 (实拍图是按时间连续的, 均匀抽才不会全抽到同一段)"""
    hold = [p for i, p in enumerate(paths) if i % every == 0]
    train = [p for i, p in enumerate(paths) if i % every != 0]
    return train, hold


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lprnet-dir', default=DEFAULT_LPRNET_DIR)
    ap.add_argument('--weights', default=None)
    ap.add_argument('--data-dirs', default=os.path.join(_HERE, 'out', 'collect2'))
    ap.add_argument('--epochs', type=int, default=15)
    ap.add_argument('--lr', type=float, default=1e-4)
    ap.add_argument('--batch', type=int, default=32)
    ap.add_argument('--replay-per-epoch', type=int, default=800,
                    help='每轮混进来的 CCPD 训练图张数 (0 = 不混)')
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--out', default=os.path.join(_HERE, 'out', 'finetune', 'finetuned_s3.pth'))
    a = ap.parse_args()

    lprnet_dir = os.path.abspath(a.lprnet_dir)
    if lprnet_dir not in sys.path:
        sys.path.insert(0, lprnet_dir)

    import numpy as np
    import torch
    import torch.nn as nn
    from data.load_data import CHARS, CHARS_DICT
    from model.LPRNet import build_lprnet

    weights = a.weights or os.path.join(lprnet_dir, 'weights', 'Final_LPRNet_model.pth')
    rng = random.Random(a.seed)
    torch.manual_seed(a.seed)

    # ---- 数据 ----
    shoot = []
    for d in sorted(os.listdir(a.data_dirs)):
        sub = os.path.join(a.data_dirs, d)
        if os.path.isdir(sub):
            shoot += collect([sub])
    shoot = [p for p in shoot if all(c in CHARS_DICT for c in label_of(p))]
    tr_shoot, va_shoot = split_holdout(shoot, 5)
    replay_all = collect([os.path.join(lprnet_dir, 'data', 'official_train')])
    replay_all = [p for p in replay_all if all(c in CHARS_DICT for c in label_of(p))]
    val_dir = os.path.join(lprnet_dir, 'data', 'official_val')
    va_ccpd = [p for p in collect([val_dir]) if all(c in CHARS_DICT for c in label_of(p))]
    print('实拍: 训练 %d / 留出 %d | CCPD replay %d | CCPD 验证 %d'
          % (len(tr_shoot), len(va_shoot), len(replay_all), len(va_ccpd)))
    print('实拍标签分布:', dict(Counter(label_of(p) for p in shoot)))

    net = build_lprnet(lpr_max_len=8, phase=True, class_num=len(CHARS), dropout_rate=0.5)
    net.load_state_dict(torch.load(weights, map_location='cpu'))
    print('已加载权重:', weights)

    ctc = nn.CTCLoss(blank=len(CHARS) - 1, reduction='mean')
    opt = torch.optim.RMSprop(net.parameters(), lr=a.lr, alpha=0.9, eps=1e-8,
                              momentum=0.9, weight_decay=2e-5)

    def evaluate(paths):
        import cv2
        was = net.training
        net.eval()
        ok = 0
        with torch.no_grad():
            for p in paths:
                img = load_bgr(p)
                if img is None:
                    continue
                if img.shape[1] != IMG_W or img.shape[0] != IMG_H:
                    img = cv2.resize(img, (IMG_W, IMG_H))
                x = torch.from_numpy(np.ascontiguousarray(to_tensor(img)))[None]
                logits = net(x)[0].numpy()          # [C, T]
                seq = logits.argmax(0)
                res, prev = [], -1
                for c in seq:
                    if c != prev and c != len(CHARS) - 1:
                        res.append(CHARS[c])
                    prev = c
                if ''.join(res) == label_of(p):
                    ok += 1
        if was:
            net.train()
        return ok, len(paths)

    o1, n1 = evaluate(va_shoot)
    o2, n2 = evaluate(va_ccpd)
    print('微调前基线: 实拍留出 %d/%d=%.1f%% | CCPD验证 %d/%d=%.1f%%'
          % (o1, n1, 100.0 * o1 / max(1, n1), o2, n2, 100.0 * o2 / max(1, n2)))
    best = (o2 / max(1, n2), o1 / max(1, n1))
    def batches(pool, bs):
        for i in range(0, len(pool) - bs + 1, bs):
            yield pool[i:i + bs]

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    for ep in range(1, a.epochs + 1):
        pool = list(tr_shoot)
        if a.replay_per_epoch > 0:
            pool += rng.sample(replay_all, min(a.replay_per_epoch, len(replay_all)))
        rng.shuffle(pool)
        net.train()
        t0 = time.time()
        losses = []
        for b in batches(pool, a.batch):
            imgs, labels, lengths = [], [], []
            for p in b:
                img = load_bgr(p)
                if img is None:
                    continue
                if img.shape[1] != IMG_W or img.shape[0] != IMG_H:
                    import cv2
                    img = cv2.resize(img, (IMG_W, IMG_H))
                img = augment(img, rng)
                imgs.append(to_tensor(img))
                lab = [CHARS_DICT[c] for c in label_of(p)]
                labels += lab
                lengths.append(len(lab))
            if not imgs:
                continue
            images = torch.from_numpy(np.ascontiguousarray(np.stack(imgs, 0)))
            targets = torch.from_numpy(np.asarray(labels, dtype=np.int64))
            logits = net(images)
            log_probs = logits.permute(2, 0, 1).log_softmax(2)
            loss = ctc(log_probs, targets,
                       input_lengths=tuple([logits.shape[2]] * len(lengths)),
                       target_lengths=tuple(lengths))
            opt.zero_grad()
            if not torch.isfinite(loss):
                continue
            loss.backward()
            opt.step()
            losses.append(loss.item())
        o1, n1 = evaluate(va_shoot)
        o2, n2 = evaluate(va_ccpd)
        a1, a2 = o1 / max(1, n1), o2 / max(1, n2)
        print('第 %2d/%d 轮  loss=%.3f  %4.1fs | 实拍留出 %d/%d=%.1f%% | CCPD验证 %d/%d=%.1f%%'
              % (ep, a.epochs, sum(losses) / max(1, len(losses)), time.time() - t0,
                 o1, n1, 100 * a1, o2, n2, 100 * a2))
        # 以"两边都不掉"为目标: 先保证 CCPD 不低于基线 -2%, 再比实拍
        if a2 >= 0.84 and a1 >= best[1]:
            best = (a2, a1)
            torch.save(net.state_dict(), a.out)
            print('   -> 存盘 (实拍 %.1f%% / CCPD %.1f%%)' % (100 * a1, 100 * a2))
    if os.path.isfile(a.out):
        print('完成. 已存最好一版: 实拍留出 %.1f%% / CCPD验证 %.1f%% -> %s'
              % (100 * best[1], 100 * best[0], a.out))
    else:
        print('完成. 但没有任何一轮同时满足 (CCPD验证 >= 84% 且 实拍留出不低于基线), 没有存盘。')
        print('      最好一版是第 3 行那个基线: 实拍留出 %.1f%% / CCPD验证 %.1f%%。'
              % (100 * best[1], 100 * best[0]))
        print('      这时正确做法是去采 P5.16 模式 2 的模型输入块, 而不是硬训 (见文档第二十八章)。')


if __name__ == '__main__':
    main()