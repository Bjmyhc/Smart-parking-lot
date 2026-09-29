# -*- coding: utf-8 -*-
"""
采图整理: 把 BY 串口助手存下来的预览图, 变成 LPRNet 能直接吃的训练素材.

做三件事:
  1) 在整帧里找车牌 -> 最小外接旋转矩形
  2) 按端侧同样的规则外扩/收边后摆正裁出来 (warpPerspective, 与训练链路一致)
  3) 兜底: 万一混进了"按 BOOT 抓的、画着绿框的那一帧", 在裁好的图上把绿框擦掉
     -> 生成 <标签>_NNN.jpg

用法:
  python prep_collect.py <输入目录> <标签> <输出目录>
例: python prep_collect.py saved/images 粤T666FP out/train
标签就是文件名(不含扩展名)里 "-" 或 "_" 之前的那段, 与训练脚本 load_data.py 的约定一致.

P5.17 (2026-09-29) 重写了"找车牌", 记录一下为什么:
  老版本用 cv2.inRange(H 100~124, S>=43) 取最大轮廓再 minAreaRect. 屏幕翻拍的蓝牌
  被冲淡, 掩码在车牌内部只剩零星碎片 —— 实测 97 张里最大轮廓的覆盖率只有 5%~36%,
  于是"最大的一块"只是车牌的某个局部, 裁出来是"放大的一角". 这批图的离线正确率
  因此只有 3/64, 看起来像"端侧不行", 其实是裁错了.
  新版本三步:
    1) 蓝占优掩码: B-R > 25 且 B > 90  (对"发白的蓝"也成立, 不要求饱和度)
    2) 取最大【连通块】(不是最大轮廓), 再闭运算把白字造成的洞补上
    3) minAreaRect 之后显式处理 90 度翻转 (w<h 时换轴并 +90 度),
       宽高不要靠 boxPoints 的角点顺序推 —— 角度接近 -90 度时那个顺序会翻, 会裁成竖条
  改完同一批 97 张: 离线正确率 3/64 -> 62% (细节见移植进度文档第 27 章)
"""
import os, sys, glob, argparse
import numpy as np
import cv2

# 与固件 main.cpp 的 ROI_CROP_PAD 语义一致: 框外扩/收缩的倍数 (1.0 = 蓝区紧贴边)
SCALE_X = 1.05      # 横向: 实测扫描的最优区间 (94 宽的张量里, 字符整体偏窄一点最合适)
SCALE_Y = 0.95      # 纵向: 同上
MIN_AREA_PCT = 0.03 # 连通块面积小于整帧的 3% 就不认 (预览图里车牌占 25%~35%)


def plate_quad(img):
    """蓝占优 -> 最大连通块 -> 闭运算补洞 -> 最小外接旋转矩形. 返回 ((cx,cy),(w,h),ang) 或 None"""
    b = img[:, :, 0].astype(np.int16)
    r = img[:, :, 2].astype(np.int16)
    m = ((b - r > 25) & (b > 90)).astype(np.uint8) * 255
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((7, 7), np.uint8))
    m = cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    n, lab, stats, _ = cv2.connectedComponentsWithStats(m, 8)
    if n <= 1:
        return None
    k = 1 + int(np.argmax(stats[1:, 4]))
    if stats[k, 4] < MIN_AREA_PCT * img.shape[0] * img.shape[1]:
        return None
    comp = (lab == k).astype(np.uint8) * 255
    comp = cv2.morphologyEx(comp, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
    cnts, _ = cv2.findContours(comp, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    c = max(cnts, key=cv2.contourArea)
    (cx, cy), (w, h), ang = cv2.minAreaRect(c)
    if w < h:                      # 统一成"长边在前", 否则下面的角点顺序会把宽高弄反
        w, h, ang = h, w, ang + 90
    return (cx, cy), (w, h), ang


def crop_plate(img, quad, sx=SCALE_X, sy=SCALE_Y, outw=282):
    """按旋转矩形摆正裁出车牌. sx/sy 是相对蓝区外接矩形的缩放 (外扩 >1, 收边 <1)"""
    (cx, cy), (w, h), ang = quad
    w *= sx
    h *= sy
    box = np.float32(cv2.boxPoints(((cx, cy), (w, h), ang)))
    s = box.sum(axis=1)
    d = np.diff(box, axis=1)
    q = np.zeros((4, 2), np.float32)
    q[0] = box[np.argmin(s)]       # 左上
    q[1] = box[np.argmin(d)]       # 右上
    q[2] = box[np.argmax(s)]       # 右下
    q[3] = box[np.argmax(d)]       # 左下
    ww = int(round(max(np.linalg.norm(q[0] - q[1]), np.linalg.norm(q[2] - q[3]))))
    hh = int(round(max(np.linalg.norm(q[0] - q[3]), np.linalg.norm(q[1] - q[2]))))
    if ww < 20 or hh < 10:
        return None
    dst = np.float32([[0, 0], [ww - 1, 0], [ww - 1, hh - 1], [0, hh - 1]])
    M = cv2.getPerspectiveTransform(q, dst)
    warped = cv2.warpPerspective(img, M, (ww, hh), borderMode=cv2.BORDER_REPLICATE)
    outh = max(8, int(round(outw * hh / max(ww, 1))))
    return cv2.resize(warped, (outw, outh), interpolation=cv2.INTER_AREA)


def erase_green_box(img):
    """把固件画的纯绿框连 JPEG 渗色一起抹掉.

    只在"蓝牌"的裁剪图上调用 —— 绿牌底色本身是绿的, 会被一起抹掉.
    阈值不能太紧: 绿线是 RGB(0,255,0), JPEG 一压就成了 1~2 px 的发绿渗色.
    (没绿像素时直接原样返回, 所以对干净的连续帧是零开销.)
    """
    b, g, r = img[:, :, 0].astype(int), img[:, :, 1].astype(int), img[:, :, 2].astype(int)
    m = ((g > 90) & (g - b > 25) & (g - r > 35)).astype(np.uint8) * 255
    if m.sum() == 0:
        return img, 0
    m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=2)
    return cv2.inpaint(img, m, 5, cv2.INPAINT_TELEA), int((m > 0).sum())


def is_blue_plate(crop):
    """这张裁剪图是蓝牌还是绿牌? 只比谁多, 免得绿牌的底色被擦绿框那一步削掉."""
    b, g = crop[:, :, 0].astype(int), crop[:, :, 1].astype(int)
    return int((b > g + 15).sum()) > int((g > b + 15).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("indir")
    ap.add_argument("label")
    ap.add_argument("outdir")
    ap.add_argument("--outw", type=int, default=282)
    ap.add_argument("--scale-x", type=float, default=SCALE_X)
    ap.add_argument("--scale-y", type=float, default=SCALE_Y)
    ap.add_argument("--montage", default="")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    files = sorted(sum([glob.glob(os.path.join(a.indir, e)) for e in ("*.jpg", "*.png", "*.jpeg")], []))
    if not files:
        print("输入目录里没有图片:", a.indir)
        return
    ok, skip, tiles = 0, 0, []
    for p in files:
        img = cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            skip += 1
            continue
        q = plate_quad(img)
        if q is None:
            skip += 1
            continue
        crop = crop_plate(img, q, a.scale_x, a.scale_y, a.outw)
        if crop is None:
            skip += 1
            continue
        if is_blue_plate(crop):
            crop, _ = erase_green_box(crop)      # 绿牌不能擦, 会把底色一起抹掉
        out = os.path.join(a.outdir, "%s_%03d.jpg" % (a.label, ok + 1))
        cv2.imencode(".jpg", crop, [int(cv2.IMWRITE_JPEG_QUALITY), 95])[1].tofile(out)
        ok += 1
        tiles.append(crop)
    print("输入 %d 张 -> 出图 %d 张, 跳过 %d 张 (找不到车牌)" % (len(files), ok, skip))
    if a.montage and tiles:
        tiles = tiles[:40]
        W = max(t.shape[1] for t in tiles)
        canvas = np.full((sum(t.shape[0] + 4 for t in tiles), W, 3), 30, np.uint8)
        y = 0
        for t in tiles:
            canvas[y:y + t.shape[0], 0:t.shape[1]] = t
            y += t.shape[0] + 4
        cv2.imencode(".png", canvas)[1].tofile(a.montage)
        print("预览拼图:", a.montage)


if __name__ == "__main__":
    main()