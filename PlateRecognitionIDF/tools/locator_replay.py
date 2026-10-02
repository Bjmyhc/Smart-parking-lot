# -*- coding: utf-8 -*-
"""locator_replay.py —— 在 PC 上复算固件的"车牌 ROI 定位"那一段(主工程 main.cpp 的 roi_locate)。

为什么需要它
    板端改一个阈值 = 编译 + 烧录 + 重新拍, 一轮好几分钟。而"这个阈值会套出什么样的框"其实
    完全能在 PC 上算出来。本脚本与固件逐段对应, 常量同名同值:
        逐像素判牌色 -> 1/4 粗网格累加 -> 闭/开(4x1) -> 连通域(union-find, 4 邻域)
        -> 按面积取前 32 名 -> 每候选: 轴对齐比例 + PCA 旋转拟合 + 有效填充率 + 得分
        -> 严格档(面积/比例/填充) -> 兜底档(只看面积) -> 贴边处理 -> 去边
    于是"改哪个阈值会怎样"几秒就能看几十张, 不用碰板子。

与固件的已知差异(都不影响结论)
    1) 固件吃 RGB565 大端, 这里吃 8bit BGR (量化误差 ~1/32)。
    2) 固件固定 640x480 / 步长 4; 这里按输入尺寸自适应选步长(320x240 -> 步长 2),
       保持"网格 160x120 / 一格 >=25% 像素是牌色"这两条语义不变。
    3) 这里只复算"定位", 不做裁剪、摆正采样和光度归一化。

用法
    # 单张: 打印候选表
    python locator_replay.py 图.jpg

    # 一整个文件夹(递归): 每张一张诊断图 + 一份 CSV + 末尾汇总
    python locator_replay.py 照片目录/ --out-dir tools/out/replay --csv tools/out/replay.csv

    # 试阈值: 蓝牌饱和度 35->65, 并给候选加 45% 占屏上限
    python locator_replay.py 照片目录/ --s-blue 65 --max-area-pct 45

    # 扫阈值: 一次列出多档 S 下"能挑出像车牌的框"的帧数 —— 调参主力
    python locator_replay.py 照片目录/ --sweep-s 35,50,65,80

注意
    请喂**模式 0 的干净预览图**。模式 1 的预览图上有固件画的绿框, 那圈纯绿会被"绿牌色"
    判成车牌色, 把定位带偏(本脚本默认会先把那种纯绿线抹掉, 见 --keep-green-box)。
"""

import argparse
import csv
import math
import os
import sys

import cv2
import numpy as np

# ---------------- 固件常量 (main.cpp, 改名请同步) ----------------
GRID_W, GRID_H, GRID_N = 160, 120, 19200
MAX_SLOTS = 32
MIN_AREA_RATIO = 0.002
RATIO_LO, RATIO_HI, RATIO_IDEAL = 2.0, 6.5, 3.4
MIN_FILL = 0.60
FIT_MIN_FILL = 0.35
TRIM_X, TRIM_Y = 0.01, 0.03
H_LO, H_HI = 180, 270
H_G_LO, H_G_HI = 70, 170
V_MIN = 46
V_MIN_CUR = V_MIN                  # 运行时可调(固件就是 46), main()/sweep 里按 --v-min 更新
S_BLUE_DEF, S_GREEN_DEF = 35, 43
S_BLUE_MIN = min(S_BLUE_DEF, S_GREEN_DEF)   # plate_mask 的粗筛门限, main() 按 --s-blue/--s-green 更新


def cdiv_arr(a, b):
    """C 的整数除法(向零截断)。b 恒为正。"""
    q = np.abs(a) // np.abs(b)
    return np.where(a < 0, -q, q)


def plate_mask(bgr, s_blue, s_green, h_lo, h_hi):
    """逐像素"是不是车牌颜色"。与固件 roi_is_plate_color 同规则(同优先级、同截断)。"""
    b = bgr[:, :, 0].astype(np.int32)
    g = bgr[:, :, 1].astype(np.int32)
    r = bgr[:, :, 2].astype(np.int32)
    v = np.maximum(np.maximum(r, g), b)
    m = np.minimum(np.minimum(r, g), b)
    delta = v - m

    ok = (v >= V_MIN_CUR) & (delta > 0)
    ok &= (255 * delta >= S_BLUE_MIN * v)          # 固件的饱和度粗筛(用最低的那条下限)
    safe = np.maximum(delta, 1)
    s = (255 * delta) // np.maximum(v, 1)

    deg_r = 60 * cdiv_arr(g - b, safe)
    deg_r = np.where(deg_r < 0, deg_r + 360, deg_r)
    deg_g = 60 * cdiv_arr(b - r, safe) + 120
    deg_b = 60 * cdiv_arr(r - g, safe) + 240
    deg = np.select([v == r, v == g], [deg_r, deg_g], default=deg_b)   # 与 if/elif/else 同优先级

    blue = (deg >= h_lo) & (deg <= h_hi) & (s >= s_blue)
    green = (deg >= H_G_LO) & (deg <= H_G_HI) & (s >= s_green)
    return ok & (blue | green)


def morph_h(binimg, erode):
    """网格上的水平形态学, 结构元 4x1, 锚点偏移 -2..+1; 出界位置不参与(等价 OpenCV 边界)。"""
    acc = np.ones(binimg.shape, bool) if erode else np.zeros(binimg.shape, bool)
    for k in (-2, -1, 0, 1):
        valid = np.zeros(binimg.shape, bool)
        shifted = np.zeros(binimg.shape, bool)
        if k < 0:
            shifted[:, :k] = binimg[:, -k:]
            valid[:, :k] = True
        elif k > 0:
            shifted[:, k:] = binimg[:, :-k]
            valid[:, k:] = True
        else:
            shifted = binimg.copy()
            valid[:] = True
        if erode:
            acc &= np.where(valid, shifted, True)
        else:
            acc |= np.where(valid, shifted, False)
    return acc


def label4(binimg):
    """4 邻域连通域标记。返回 (标签图, 标签->格数, 标签 0 为背景)。"""
    h, w = binimg.shape
    lab = np.zeros((h, w), np.int32)
    sizes = [0]
    cur = 0
    ys, xs = np.where(binimg)
    for y0, x0 in zip(ys.tolist(), xs.tolist()):
        if lab[y0, x0]:
            continue
        cur += 1
        n = 0
        stack = [(y0, x0)]
        lab[y0, x0] = cur
        while stack:
            y, x = stack.pop()
            n += 1
            if y > 0 and binimg[y - 1, x] and not lab[y - 1, x]:
                lab[y - 1, x] = cur
                stack.append((y - 1, x))
            if y + 1 < h and binimg[y + 1, x] and not lab[y + 1, x]:
                lab[y + 1, x] = cur
                stack.append((y + 1, x))
            if x > 0 and binimg[y, x - 1] and not lab[y, x - 1]:
                lab[y, x - 1] = cur
                stack.append((y, x - 1))
            if x + 1 < w and binimg[y, x + 1] and not lab[y, x + 1]:
                lab[y, x + 1] = cur
                stack.append((y, x + 1))
        sizes.append(n)
    return lab, np.array(sizes)


def fit_rotated(cell, comp, need, step, w, h, fit_min_fill):
    """PCA 拟合旋转矩形(等价 cv2.minAreaRect)。返回 (fit 或 None, 原因字符串)。"""
    ys, xs = np.where(comp & (cell >= need))
    n = len(xs)
    if n < 30:
        return None, '车牌色格数太少 (<30 格)'
    mx, my = xs.mean(), ys.mean()
    dx = xs - mx
    dy = ys - my
    cxx = float((dx * dx).mean())
    cyy = float((dy * dy).mean())
    cxy = float((dx * dy).mean())
    theta = 0.5 * math.atan2(2.0 * cxy, cxx - cyy)
    ux, uy = math.cos(theta), math.sin(theta)
    u = dx * ux + dy * uy
    vv = -dx * uy + dy * ux
    umin, umax = float(u.min()), float(u.max())
    vmin, vmax = float(vv.min()), float(vv.max())
    uw, vh = umax - umin, vmax - vmin
    if uw < 6.0 or vh < 3.0:
        return None, '旋转矩形太小 (要 >=24x12 像素)'
    ratio = uw / vh
    fill = n / ((uw + 1.0) * (vh + 1.0))
    if ratio < 1.2 or ratio > 8.0:
        return None, '旋转长宽比越界 (要 1.2~8.0)'
    if fill < fit_min_fill:
        return None, '旋转矩形填充不足 (<%.0f%%)' % (fit_min_fill * 100)
    um0, um1 = umin + uw * TRIM_X, umax - uw * TRIM_X
    vm0, vm1 = vmin + vh * TRIM_Y, vmax - vh * TRIM_Y
    if um1 <= um0 or vm1 <= vm0:
        return None, '去边后区间为空'
    uu = [um0, um1, um1, um0]
    vv4 = [vm0, vm0, vm1, vm1]
    cxs, cys = [], []
    for k in range(4):
        gx = mx + uu[k] * ux - vv4[k] * uy
        gy = my + uu[k] * uy + vv4[k] * ux
        cxs.append(min(max((gx + 0.5) * step, 0.0), w - 1))
        cys.append(min(max((gy + 0.5) * step, 0.0), h - 1))
    st = min(range(4), key=lambda k: cxs[k] + cys[k])          # (x+y) 最小的角 = 左上
    nxt = (st + 1) % 4
    if cxs[(st + 3) % 4] > cxs[nxt]:
        nxt = (st + 3) % 4
    order = [st, nxt, (st + 2) % 4, (nxt + 2) % 4]
    return {
        'ratio': ratio, 'fill': fill, 'deg': math.degrees(theta),
        'uw': uw, 'vh': vh, 'np': n,
        'qx': [cxs[i] for i in order], 'qy': [cys[i] for i in order],
    }, '通过'


def locate(work_in, args):
    """复算一帧的定位。返回 (工作图, 掩码, info)。"""
    h, w = work_in.shape[:2]
    step = args.grid_step
    if step is None:
        step = max(1, min(int(round(w / float(GRID_W))), 12))
    tw, th = GRID_W * step, GRID_H * step
    aspect_in = w / float(h)
    if (w, h) != (tw, th):
        interp = cv2.INTER_AREA if (tw < w or th < h) else cv2.INTER_LINEAR
        work_in = cv2.resize(work_in, (tw, th), interpolation=interp)
        h, w = th, tw
    work = work_in

    mask = plate_mask(work, args.s_blue, args.s_green, args.h_lo, args.h_hi)
    need = (step * step + 3) // 4
    cell = mask.reshape(GRID_H, step, GRID_W, step).sum(axis=(1, 3)).astype(np.int32)

    sum_r, sum_g, sum_b = (float(work[:, :, i].sum()) for i in (2, 1, 0))
    inv = 1.0 / (w * h)
    mx3 = work.max(axis=2)
    info = {
        'src_size': (args.orig_w, args.orig_h), 'work_size': (w, h), 'step': step,
        'aspect_in': aspect_in, 'aspect_work': w / float(h),
        'mean': (sum_r * inv, sum_g * inv, sum_b * inv),
        'color_px_pct': 100.0 * float(mask.sum()) * inv,
        'over_pct': 100.0 * float((mx3 >= 250).sum()) * inv,
        'dark_pct': 100.0 * float((mx3 <= 5).sum()) * inv,
        'cands': [], 'chosen': None, 'verdict': None, 'note': '',
    }

    binimg = cell >= need
    info['color_cells'] = int(binimg.sum())
    if info['color_cells'] == 0:
        info['verdict'] = '没有牌色'
        info['note'] = '画面里没有一块车牌色 —— 车牌太小/太远/太暗, 或者偏色'
        return work, mask, info

    binimg = morph_h(morph_h(binimg, False), True)      # 闭 = 膨胀+腐蚀
    binimg = morph_h(morph_h(binimg, True), False)      # 开 = 腐蚀+膨胀

    lab, sizes = label4(binimg)
    roots = list(range(1, len(sizes)))
    if len(roots) <= MAX_SLOTS:
        sel = roots
    else:
        sel = sorted(roots, key=lambda r: (-int(sizes[r]), r))[:MAX_SLOTS]

    min_cells = int(GRID_N * args.min_area_ratio)
    for k, r in enumerate(sel):
        comp = lab == r
        ys, xs = np.where(comp)
        sl = {'x1': int(xs.min()), 'x2': int(xs.max()), 'y1': int(ys.min()), 'y2': int(ys.max()),
              'area': int(sizes[r])}
        bw = float(sl['x2'] - sl['x1'] + 1)
        bh = float(sl['y2'] - sl['y1'] + 1)
        axis_ratio = bw / bh
        fit, fit_why = fit_rotated(cell, comp, need, step, w, h, args.fit_min_fill)
        ratio = fit['ratio'] if fit else axis_ratio
        dens = sl['area'] / (bw * bh)
        denseff = fit['fill'] if fit else dens
        score = sl['area'] / (1.0 + abs(ratio - RATIO_IDEAL))
        area_pct = 100.0 * bw * bh / GRID_N

        ok = okr = capped = False
        if sl['area'] < min_cells:
            status = '连通域太小'
        elif ratio < args.ratio_lo or ratio > args.ratio_hi:
            status = '旋转宽高比越界' if fit else '宽高比越界(拟合失败, 退回轴对齐)'
        else:
            okr = True
            if denseff < args.min_fill:
                status = '有效填充不足'
            else:
                ok = True
                status = '通过'
        if args.max_area_pct > 0.0 and area_pct > args.max_area_pct:
            capped = True                       # 硬性排除: 严格档和兜底档都不再要它
            ok = okr = False
            status = '%s -> 被占屏上限 %.0f%% 挡下 (占屏 %.0f%%)' % (status, args.max_area_pct, area_pct)
        info['cands'].append({
            'k': k, 'area': sl['area'], 'box_cells': (bw, bh),
            'box_px': (int(bw * step), int(bh * step)), 'box_xy': (sl['x1'] * step, sl['y1'] * step),
            'axis_ratio': axis_ratio, 'ratio': ratio, 'dens': dens, 'denseff': denseff,
            'score': score, 'area_pct': area_pct, 'ok': ok, 'okr': okr, 'capped': capped,
            'fit': fit, 'fit_why': fit_why, 'why': status, 'sl': sl,
        })

    cands = info['cands']
    strict = [c for c in cands if c['ok']]
    fallback = False
    if strict:
        best = max(strict, key=lambda c: c['score'])
    else:
        pool = [] if args.no_fallback else [c for c in cands if c['area'] >= min_cells and not c['capped']]
        best = max(pool, key=lambda c: c['score']) if pool else None
        fallback = best is not None

    if best is None:
        miss = max(cands, key=lambda c: c['area']) if cands else None
        info['verdict'] = '没有候选'
        if miss:
            info['note'] = ('有车牌色, 但没有一块像车牌的长方形 —— 最接近的候选: 色格%d 比例%.2f 填充%.0f%%'
                            ' | 淘汰于[%s] | 门槛 色格>=%d 比例%.1f~%.1f 填充>=%.0f%%'
                            % (miss['area'], miss['ratio'], miss['denseff'] * 100, miss['why'],
                               min_cells, args.ratio_lo, args.ratio_hi, args.min_fill * 100))
        else:
            info['note'] = '有车牌色, 但一个车牌色连通域都没有 —— 太散'
        return work, mask, info

    sl = best['sl']
    touch = []
    if sl['x1'] == 0:
        touch.append('左')
    if (sl['x2'] + 1) * step >= w:
        touch.append('右')
    if sl['y1'] == 0:
        touch.append('上')
    if (sl['y2'] + 1) * step >= h:
        touch.append('下')
    if touch:
        if fallback:
            info['note'] = '兜底档: 候选贴到画面%s边缘也照用 —— 结果交给结果闸门判' % ''.join(touch)
        else:
            alt = None
            for c in cands:
                if c is best or not c['ok']:
                    continue
                s2 = c['sl']
                if s2['x1'] == 0 or (s2['x2'] + 1) * step >= w or s2['y1'] == 0 or (s2['y2'] + 1) * step >= h:
                    continue
                if alt is None or c['score'] > alt['score']:
                    alt = c
            if alt is not None:
                info['note'] = '选中的候选%d 贴边 -> 改选不贴边的候选%d' % (best['k'], alt['k'])
                best = alt
                sl = best['sl']
            elif not args.no_edge_skip:
                info['verdict'] = '贴边拒绝'
                info['note'] = ('车牌贴到画面%s边缘被切掉了(也没有不贴边的候选可换) —— '
                                '把它完整移进画面, 四周留一成余量' % ''.join(touch))
                return work, mask, info

    bw, bh = sl['x2'] - sl['x1'] + 1, sl['y2'] - sl['y1'] + 1
    box = {'x1': sl['x1'] * step, 'y1': sl['y1'] * step,
           'x2': (sl['x2'] + 1) * step, 'y2': (sl['y2'] + 1) * step,
           'area_pct': 100.0 * bw * bh / GRID_N}
    if best['fit']:
        box['area_pct'] = 100.0 * best['fit']['uw'] * best['fit']['vh'] / GRID_N
    tw_, th_ = box['x2'] - box['x1'], box['y2'] - box['y1']
    box['x1'] += int(tw_ * TRIM_X); box['x2'] -= int(tw_ * TRIM_X)
    box['y1'] += int(th_ * TRIM_Y); box['y2'] -= int(th_ * TRIM_Y)
    if not best['fit'] and (box['x2'] - box['x1'] < 12 or box['y2'] - box['y1'] < 6):
        info['verdict'] = '框太小'
        info['note'] = '找到的框太小 —— 车牌离得太远'
        return work, mask, info
    box['x1'] = max(0, box['x1']); box['y1'] = max(0, box['y1'])
    box['x2'] = min(w, box['x2']); box['y2'] = min(h, box['y2'])
    best['box'] = box
    info['chosen'] = best
    info['verdict'] = '兜底档' if fallback else '严格档'
    return work, mask, info


def draw(work, mask, info, title):
    vis = work.copy()
    vis[mask] = (0.45 * vis[mask] + 0.55 * np.array([0, 0, 255])).astype(np.uint8)
    for c in info['cands']:
        if c is info['chosen']:
            continue
        color = (255, 255, 0) if c['ok'] else ((0, 128, 255) if c['capped'] else (128, 128, 255))
        x0, y0 = c['box_xy']
        cv2.rectangle(vis, (x0, y0), (x0 + c['box_px'][0] - 1, y0 + c['box_px'][1] - 1), color, 1)
    ch = info['chosen']
    if ch is not None:
        if ch['fit']:
            qx = np.array(ch['fit']['qx'], np.int32)
            qy = np.array(ch['fit']['qy'], np.int32)
            cv2.polylines(vis, [np.stack([qx, qy], axis=1)], True, (0, 255, 255), 2)
        b = ch['box']
        cv2.rectangle(vis, (b['x1'], b['y1']), (b['x2'], b['y2']), (0, 255, 0), 2)
    cv2.rectangle(vis, (0, 0), (work.shape[1], 22), (0, 0, 0), -1)
    cv2.putText(vis, title, (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
    return vis


def report(name, info, args):
    out = []
    w, h = info['work_size']
    sr, sg, sb = info['mean']
    out.append('=' * 96)
    out.append('图: %s   源 %dx%d -> 工作 %dx%d (格步长 %d)'
               % (name, info['src_size'][0], info['src_size'][1], w, h, info['step']))
    if abs(info['aspect_in'] - info['aspect_work']) / info['aspect_work'] > 0.05:
        out.append('⚠ 源图宽高比 %.2f 被拉成 %.2f (工作网格固定 4:3) —— 框的几何会有偏差, 尽量用 4:3 的图'
                   % (info['aspect_in'], info['aspect_work']))
    out.append('整帧均值: R=%.0f G=%.0f B=%.0f | 车牌色像素 %.1f%% | 车牌色格 %d/%d = %.1f%%'
               % (sr, sg, sb, info['color_px_pct'], info['color_cells'], GRID_N,
                  100.0 * info['color_cells'] / GRID_N))
    exp = '正常'
    if info['over_pct'] > 8.0:
        exp = '明显过曝 —— 车牌色会被洗淡'
    elif info['dark_pct'] > 25.0:
        exp = '整体偏暗 —— 蓝底可能压不出色相'
    out.append('整帧曝光: 过曝 %.1f%% | 死黑 %.1f%% | %s' % (info['over_pct'], info['dark_pct'], exp))
    if not info['cands']:
        out.append('判定: [%s] %s' % (info['verdict'], info['note']))
        return out

    out.append('-' * 96)
    out.append('候选表 (按连通域面积取前 %d 名; 面积门槛 %d 格, 比例 %.1f~%.1f, 有效填充 >=%.0f%%):'
               % (MAX_SLOTS, int(GRID_N * args.min_area_ratio), args.ratio_lo, args.ratio_hi, args.min_fill * 100))
    out.append(' %-3s %7s %11s %7s %7s %7s %8s %8s %7s  %s'
               % ('k', '色格', '轴框(px)', '轴比例', '旋转比', '倾角', '旋转填充', '有效填充', '得分', '判定'))
    cands = info['cands']
    by_area = sorted(cands, key=lambda c: -c['area'])[:6]
    show = {c['k'] for c in by_area} | {c['k'] for c in cands if c['ok'] or c['okr']}
    if info['chosen'] is not None:
        show.add(info['chosen']['k'])
    shown = [c for c in cands if c['k'] in show]
    for c in shown:
        f = c['fit']
        mark = ' <= 选中' + ('(兜底)' if info['verdict'] == '兜底档' else '') if c is info['chosen'] else ''
        why = c['why']
        if not f and c['area'] >= int(GRID_N * args.min_area_ratio):
            why += ' | 拟合失败: %s' % c['fit_why']
        out.append(' %-3d %7d %11s %7.2f %7s %7s %8s %7.0f%% %7.0f  %s%s'
                   % (c['k'], c['area'], '%dx%d' % c['box_px'], c['axis_ratio'],
                      ('%.2f' % f['ratio']) if f else '-',
                      ('%+.1f' % f['deg']) if f else '-',
                      ('%.0f%%' % (f['fill'] * 100)) if f else '-',
                      c['denseff'] * 100, c['score'], why, mark))
    if len(cands) > len(shown):
        out.append(' ... 另有 %d 个候选没列出(加 --verbose 看全部)' % (len(cands) - len(shown)))

    ch = info['chosen']
    if ch is not None:
        b = ch['box']
        if ch['fit']:
            rot = '成功 (倾角 %+.1f° 旋转比例 %.2f)' % (ch['fit']['deg'], ch['fit']['ratio'])
        else:
            rot = '失败 -> 这一帧只能轴对齐硬裁 (斜牌这样裁必糊)'
        out.append('-' * 96)
        out.append('判定: [%s] 框 %dx%d @(%d,%d) 占屏 %.1f%% | 旋转拟合 %s'
                   % (info['verdict'], b['x2'] - b['x1'], b['y2'] - b['y1'], b['x1'], b['y1'],
                      b['area_pct'], rot))
    if info['note']:
        out.append('说明: %s' % info['note'])
    return out


def strip_green_box(bgr):
    """抹掉固件画在预览图上的纯绿框(约 (0,255,0)), 免得它被当成绿牌色。返回抹掉的像素数。"""
    gbox = (bgr[:, :, 1] >= 200) & (bgr[:, :, 0] <= 100) & (bgr[:, :, 2] <= 100)
    if not gbox.any():
        return 0
    gbox = cv2.dilate(gbox.astype(np.uint8), np.ones((3, 3), np.uint8), iterations=1) > 0
    bgr[gbox] = 0
    return int(gbox.sum())


def load(path):
    return cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)


def collect_files(path):
    if os.path.isdir(path):
        found = []
        for root, _, names in os.walk(path):
            for n in sorted(names):
                if os.path.splitext(n)[1].lower() in ('.jpg', '.jpeg', '.png', '.bmp'):
                    found.append(os.path.join(root, n))
        return found
    return [path]


def sweep(files, args):
    """扫 --sweep-s 里每一档饱和度, 看"能挑出像车牌的框"的帧数。"""
    print('扫阈值: 只动蓝牌饱和度下限 (其余参数不变: 比例 %.1f~%.1f, 填充 >=%.0f%%, 面积 >=%d 格)'
          % (args.ratio_lo, args.ratio_hi, args.min_fill * 100, int(GRID_N * args.min_area_ratio)))
    print('%-6s %8s %8s %8s %8s %8s   %s' % ('S', '严格档', '兜底档', '贴边拒', '无候选', '无牌色', '严格档框占屏 中位/最大'))
    for s in args.sweep_s:
        args.s_blue = s
        global S_BLUE_MIN, V_MIN_CUR
        S_BLUE_MIN = min(args.s_blue, args.s_green)
        stat = {}
        areas = []
        for f in files:
            raw = load(f)
            if raw is None or raw.shape[0] < 40 or raw.shape[1] < 60:
                continue
            args.orig_w, args.orig_h = raw.shape[1], raw.shape[0]
            if not args.keep_green_box:
                raw = raw.copy()
                strip_green_box(raw)
            _, _, info = locate(raw, args)
            stat[info['verdict']] = stat.get(info['verdict'], 0) + 1
            if info['verdict'] == '严格档' and info['chosen']:
                areas.append(info['chosen']['box']['area_pct'])
        med = np.median(areas) if areas else 0.0
        mx = max(areas) if areas else 0.0
        print('%-6g %8d %8d %8d %8d %8d   %.1f%% / %.1f%% (%d 帧)'
              % (s, stat.get('严格档', 0), stat.get('兜底档', 0), stat.get('贴边拒绝', 0),
                 stat.get('没有候选', 0), stat.get('没有牌色', 0), med, mx, len(areas)))


def main():
    ap = argparse.ArgumentParser(description='在 PC 上复算固件的车牌 ROI 定位 (roi_locate)')
    ap.add_argument('path', help='图片文件, 或目录(递归找 jpg/png/bmp)')
    ap.add_argument('--out-dir', default=None, help='诊断图输出目录')
    ap.add_argument('--csv', default=None, help='汇总 CSV 输出路径')
    ap.add_argument('--verbose', action='store_true', help='打印全部候选(不只前几名)')
    ap.add_argument('--sweep-s', default=None, help='扫饱和度: 逗号分隔, 如 35,50,65,80 (只出汇总表)')
    ap.add_argument('--grid-step', type=int, default=None, help='强制格步长(默认按宽度自适应: 320x240->2, 640x480->4)')
    ap.add_argument('--s-blue', type=float, default=S_BLUE_DEF, help='蓝牌饱和度下限(固件=%d)' % S_BLUE_DEF)
    ap.add_argument('--s-green', type=float, default=S_GREEN_DEF, help='绿牌饱和度下限(固件=%d)' % S_GREEN_DEF)
    ap.add_argument('--h-lo', type=float, default=H_LO, help='色相窗口下限(固件=%d)' % H_LO)
    ap.add_argument('--h-hi', type=float, default=H_HI, help='色相窗口上限(固件=%d)' % H_HI)
    ap.add_argument('--v-min', type=float, default=V_MIN, help='亮度下限(固件=46) —— 深色背景也带蓝时, 抬高它比抬饱和度管用')
    ap.add_argument('--min-fill', type=float, default=MIN_FILL, help='严格档有效填充率下限(固件=0.60)')
    ap.add_argument('--min-area-ratio', type=float, default=MIN_AREA_RATIO, help='面积下限占屏比(固件=0.002 -> 38 格)')
    ap.add_argument('--fit-min-fill', type=float, default=FIT_MIN_FILL, help='旋转拟合内部门限(固件=0.35)')
    ap.add_argument('--ratio-lo', type=float, default=RATIO_LO, help='严格档比例下限(固件=2.0)')
    ap.add_argument('--ratio-hi', type=float, default=RATIO_HI, help='严格档比例上限(固件=6.5)')
    ap.add_argument('--max-area-pct', type=float, default=0.0, help='候选占屏上限(%%), 0=不设(固件目前不设)')
    ap.add_argument('--no-fallback', action='store_true', help='关掉兜底档(只看形状像车牌的候选)')
    ap.add_argument('--no-edge-skip', action='store_true', help='贴边也照用(不拒绝)')
    ap.add_argument('--keep-green-box', action='store_true', help='不抹掉预览图上固件画的纯绿框')
    ap.add_argument('--no-image', action='store_true', help='不出诊断图')
    args = ap.parse_args()

    global S_BLUE_MIN, V_MIN_CUR
    V_MIN_CUR = args.v_min
    S_BLUE_MIN = min(args.s_blue, args.s_green)

    files = collect_files(args.path)
    if not files:
        print('没找到图片:', args.path)
        return 1

    if args.sweep_s:
        vals = [float(x) for x in args.sweep_s.split(',') if x.strip()]
        args.sweep_s = vals
        sweep(files, args)
        return 0

    out_dir = args.out_dir
    if len(files) > 1 and not args.no_image and not out_dir:
        out_dir = os.path.join('tools', 'out', 'replay')
        print('(目录模式: 诊断图写到 %s)' % out_dir)
    if out_dir and not args.no_image:
        os.makedirs(out_dir, exist_ok=True)

    rows = []
    stat = {}
    for f in files:
        raw = load(f)
        if raw is None:
            print('解不开:', f)
            continue
        args.orig_w, args.orig_h = raw.shape[1], raw.shape[0]
        if raw.shape[0] < 40 or raw.shape[1] < 60:
            print('跳过(图太小, 像是 94x24 模型输入块, 不是整帧):', f)
            continue
        stripped = 0
        if not args.keep_green_box:
            raw = raw.copy()
            stripped = strip_green_box(raw)

        work, mask, info = locate(raw, args)
        name = os.path.basename(f) if len(files) == 1 else os.path.relpath(f, args.path)
        for line in report(name, info, args):
            print(line)
        if stripped > 0.05 * raw.shape[0] * raw.shape[1]:
            print('提示: 这张抹掉了 %d 个纯绿像素(占比偏高) —— 像是模式 1(带绿框)的图, 下次请用模式 0'
                  % stripped)

        stat[info['verdict']] = stat.get(info['verdict'], 0) + 1
        ch = info['chosen']
        rows.append({
            'file': f, 'src_w': args.orig_w, 'src_h': args.orig_h, 'step': info['step'],
            'color_pct': round(info['color_px_pct'], 2), 'color_cells': info['color_cells'],
            'n_cand': len(info['cands']), 'verdict': info['verdict'],
            'box_w': (ch['box']['x2'] - ch['box']['x1']) if ch else '',
            'box_h': (ch['box']['y2'] - ch['box']['y1']) if ch else '',
            # 2026-10-01: 补上框的位置 —— 端到端复算(裁出来喂模型)要用, 光有宽高没用
            'box_x0': ch['box']['x1'] if ch else '',
            'box_y0': ch['box']['y1'] if ch else '',
            'box_x1': ch['box']['x2'] if ch else '',
            'box_y1': ch['box']['y2'] if ch else '',
            'area_pct': round(ch['box']['area_pct'], 1) if ch else '',
            'ratio': round(ch['ratio'], 2) if ch else '',
            'fill_pct': round(ch['denseff'] * 100, 0) if ch else '',
            'rot_ok': (1 if ch['fit'] else 0) if ch else '',
            'deg': round(ch['fit']['deg'], 1) if (ch and ch['fit']) else '',
            'note': info['note'],
        })
        if out_dir and not args.no_image and ch is not None:
            b = ch['box']
            title = '%s  [%s] box %dx%d = %.1f%%' % (name, info['verdict'],
                                                     b['x2'] - b['x1'], b['y2'] - b['y1'], b['area_pct'])
            cmb = np.vstack([work, draw(work, mask, info, title)])
            stem = os.path.splitext(os.path.basename(f))[0]
            cv2.imencode('.png', cmb)[1].tofile(os.path.join(out_dir, stem + '_loc.png'))

    if len(files) > 1:
        print('=' * 96)
        print('汇总: %d 张' % sum(stat.values()))
        for k in ('严格档', '兜底档', '贴边拒绝', '没有候选', '没有牌色', '框太小'):
            if stat.get(k):
                print('   %-6s %d 张' % (k, stat[k]))
        print('   (严格档 = 形状像车牌, 定位最可信; 兜底档 = 只靠面积收下, 框容易过大)')
    if args.csv and rows:
        d = os.path.dirname(os.path.abspath(args.csv))
        if d:
            os.makedirs(d, exist_ok=True)
        with open(args.csv, 'w', newline='', encoding='utf-8-sig') as fh:
            wr = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            wr.writeheader()
            wr.writerows(rows)
        print('CSV 已写:', args.csv)
    return 0


if __name__ == '__main__':
    sys.exit(main())
