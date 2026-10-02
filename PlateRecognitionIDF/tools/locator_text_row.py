# -*- coding: utf-8 -*-
"""locator_text_row.py —— 原型: 用"一排等距白字符"来定位车牌(而不是只找蓝色连通块)。

动机 (2026-10-01):
    现在的定位地基 = "找蓝色连通块, 假设它是车牌"。难帧里蓝色块常是"车牌+屏幕边框/反光",
    而车牌自己有一半被判成"不是车牌色" -> 蓝带只能量到一半 -> 框永远是烂的。
    车牌真正独特的是 "蓝底 + 一排规整的白字", 反光洗不掉字形。

本脚本只做定位(输出框), 不跑模型 —— 先用它看框对不对, 再谈接推理。
用法:
    python locator_text_row.py 图.jpg
    python locator_text_row.py 目录 --out-dir out/row --csv out/row.csv
"""
import sys, os, glob, csv
import numpy as np
import cv2


def imread_u(p):
    return cv2.imdecode(np.fromfile(p, dtype=np.uint8), cv2.IMREAD_COLOR)


def preprocess(img, mode):
    """检测前的前置增强。实测(2026-10-01, 20 张难帧): 找到白字行的帧数
        原图 10/20 | CLAHE 10/20 | 饱和度x1.7 15/20 | CLAHE+饱和 18/20
        "高反差保留"(除以自身大尺度模糊) 0/20 —— 会把蓝底和白字的关系毁掉, 别用。
    """
    if mode in (None, "none"): return img
    if "clahe" in mode:
        lab = cv2.cvtColor(img, cv2.COLOR_BGR2LAB)
        l, a, b = cv2.split(lab)
        l = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(l)
        img = cv2.cvtColor(cv2.merge([l, a, b]), cv2.COLOR_LAB2BGR)
    if "sat" in mode:
        hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV).astype(np.float32)
        hsv[:, :, 1] = np.clip(hsv[:, :, 1] * 1.7, 0, 255)
        img = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)
    return img


def char_components(img):
    """返回白字符候选连通域 (x, y, w, h, area)。"""
    b = img[:, :, 0].astype(np.int16)
    g = img[:, :, 1].astype(np.int16)
    r = img[:, :, 2].astype(np.int16)
    v = np.maximum(np.maximum(b, g), r)
    mn = np.minimum(np.minimum(b, g), r)
    sat = (v - mn).astype(np.float32) / np.maximum(v, 1)
    white = ((v >= 125) & (sat <= 0.40)).astype(np.uint8)
    # 关键约束(2026-10-01 实测): 白像素必须**紧邻蓝色** —— 屏幕bezel/UI 的白会把车牌白字
    #   连成一大片, 20 张难帧里有 10 张因此一个字符块都分不出来; 加这条后只剩 3 张分不出。
    blue = (((b - r) > 25) & (b > 90)).astype(np.uint8)
    k = max(3, int(round(img.shape[1] / 320.0 * 7)) | 1)      # 320x240 -> 7, 640x480 -> 13
    white = white & cv2.dilate(blue, np.ones((k, k), np.uint8))
    H = img.shape[0]
    n, lab, st, cen = cv2.connectedComponentsWithStats(white, 8)
    out = []
    for i in range(1, n):
        x, y, w, h, a = st[i]
        if h < 9 or h > 0.62 * H:  continue
        if w < 4:                  continue
        ar = w / float(h)
        if ar < 0.12 or ar > 1.8:  continue          # 字符是竖长/近方形, 排除横条
        if a < 0.35 * w * h:       continue          # 形状要"实", 排除细碎反光
        if a < 60:                 continue
        out.append((float(x), float(y), float(w), float(h), float(a), float(cen[i][0]), float(cen[i][1])))
    return out


def find_rows(comps, img_w, img_h):
    """把字符候选拼成"一行"。

    2026-10-01 改版: 原来是从一个字符往右死接(间隔要 <=1.6 字高), 只要有**一个字**被并进
    背景大块就断链 -> 20 张难帧里 10 张一个行都拼不出来。现在改成:
        以一个字符为锚 -> 把所有"同高、中心线齐"的块聚成一簇 -> 在簇内按 x 排, 允许跳过
        空档(间隔 <= 2.6 字高, 即"中间缺 1~2 个字"仍算同一行) -> 从锚往右接、再往左接。
    """
    comps = sorted(comps, key=lambda c: c[0])
    rows = []
    for i, c in enumerate(comps):
        h0 = c[3]
        grp = [d for d in comps
               if abs(d[3] / h0 - 1.0) <= 0.40 and abs(d[6] - c[6]) <= 0.38 * h0]
        grp.sort(key=lambda z: z[0])
        k = grp.index(c)
        row = [c]
        last_x = c[0] + c[2]
        for d in grp[k + 1:]:
            gap = d[0] - last_x
            if gap > 2.6 * h0: break
            if gap < -0.4 * h0: continue
            row.append(d); last_x = d[0] + d[2]
        first_x = c[0]
        for d in reversed(grp[:k]):
            gap = first_x - (d[0] + d[2])
            if gap > 2.6 * h0: break
            row.insert(0, d); first_x = d[0]
        if len(row) >= 5:
            rows.append(row)
    rows.sort(key=lambda z: (-len(z), z[0][0]))
    keep = []
    for row in rows:
        x0, x1 = row[0][0], row[-1][0] + row[-1][2]
        dup = False
        for kk in keep:
            kx0, kx1 = kk[0][0], kk[-1][0] + kk[-1][2]
            if abs(x0 - kx0) < 14 and abs(x1 - kx1) < 14: dup = True; break
        if not dup: keep.append(row)
    return keep


def row_to_box(row, img_w, img_h):
    """由一行字符推出车牌框: 车牌高 ≈ 1.56×字高, 左右各留 ≈0.45×字高。"""
    h = float(np.median([c[3] for c in row]))
    cy = float(np.median([c[6] for c in row]))
    x0 = min(c[0] for c in row)
    x1 = max(c[0] + c[2] for c in row)
    bx0 = x0 - 0.45 * h; bx1 = x1 + 0.45 * h
    by0 = cy - 0.78 * h; by1 = cy + 0.78 * h
    bx0 = max(0.0, bx0); by0 = max(0.0, by0)
    bx1 = min(float(img_w), bx1); by1 = min(float(img_h), by1)
    ratio = (bx1 - bx0) / max(1e-6, (by1 - by0))
    return (bx0, by0, bx1, by1, ratio, h, len(row))


def row_score(row, box):
    h = box[5]
    hs = np.array([c[3] for c in row], np.float32)
    gap = [row[i + 1][0] - (row[i][0] + row[i][2]) for i in range(len(row) - 1)]
    gap = np.array(gap, np.float32)
    unif_h = hs.std() / max(1e-6, hs.mean())
    unif_g = gap.std() / max(1e-6, gap.mean()) if len(gap) else 9
    row_w = box[2] - 0.9 * h - box[0]
    fill = row_w / max(1e-6, h)                     # 真车牌 ≈ 4.2 (7 位)
    pen = 0.0
    if not (2.6 <= box[4] <= 4.0): pen += 2.0
    if not (2.6 <= fill <= 7.2):   pen += 1.0
    return (len(row) * 1.0 - 2.5 * unif_h - 0.8 * unif_g - pen), unif_h, unif_g, fill


def blue_frac(img, box, pad=0.0):
    x0, y0, x1, y1 = [int(round(v)) for v in box[:4]]
    x0 = max(0, x0); y0 = max(0, y0); x1 = min(img.shape[1], x1); y1 = min(img.shape[0], y1)
    if x1 - x0 < 3 or y1 - y0 < 3: return 0.0
    p = img[y0:y1, x0:x1].astype(np.int16)
    b, r = p[:, :, 0], p[:, :, 2]
    blue = (b - r > 25) & (b > 90)
    return float(blue.mean())


def detect(img, dbg=None):
    comps = char_components(img)
    rows = find_rows(comps, img.shape[1], img.shape[0])
    best = None
    for row in rows:
        box = row_to_box(row, img.shape[1], img.shape[0])
        sc, uh, ug, fill = row_score(row, box)
        bf = blue_frac(img, box)
        if bf < 0.18: continue                       # 框里得有蓝底
        sc += 2.0 * bf
        if best is None or sc > best[0]:
            best = (sc, box, row, uh, ug, fill, bf)
    if dbg is not None:
        vis = img.copy()
        for c in comps:
            cv2.rectangle(vis, (int(c[0]), int(c[1])), (int(c[0] + c[2]), int(c[1] + c[3])), (0, 255, 255), 1)
        if best:
            x0, y0, x1, y1 = [int(round(v)) for v in best[1][:4]]
            cv2.rectangle(vis, (x0, y0), (x1, y1), (0, 255, 0), 2)
            for c in best[2]:
                cv2.rectangle(vis, (int(c[0]), int(c[1])), (int(c[0] + c[2]), int(c[1] + c[3])), (0, 0, 255), 2)
        return best, np.vstack([img, vis])
    return best


def main():
    args = sys.argv[1:]
    if not args: print(__doc__); return 1
    path = args[0]; out_dir = None; csv_path = None
    if "--out-dir" in args: out_dir = args[args.index("--out-dir") + 1]
    if "--csv" in args:     csv_path = args[args.index("--csv") + 1]
    pre = "none"
    if "--pre" in args:     pre = args[args.index("--pre") + 1]
    files = [path] if os.path.isfile(path) else sorted(glob.glob(os.path.join(path, "**", "*.jpg"), recursive=True) +
                                                     glob.glob(os.path.join(path, "**", "*.png"), recursive=True))
    if out_dir: os.makedirs(out_dir, exist_ok=True)
    hit = 0; rows_out = []
    for f in files:
        img = imread_u(f)
        if img is None or img.shape[0] < 60 or img.shape[1] < 60: continue
        img = preprocess(img, pre)
        r = detect(img, dbg=out_dir)
        best, vis = r if out_dir else (r, None)
        name = os.path.basename(f)
        if best:
            sc, box, row, uh, ug, fill, bf = best
            hit += 1
            print("%-40s 框 %3.0fx%2.0f @(%3.0f,%3.0f) 比例 %.2f 字符 %d 个 | 高差 %.2f 间距差 %.2f 行宽/字高 %.2f 蓝底 %.0f%%"
                  % (name, box[2] - box[0], box[3] - box[1], box[0], box[1], box[4], len(row), uh, ug, fill, bf * 100))
            rows_out.append([name, box[0], box[1], box[2], box[3], box[4], len(row), fill, bf])
        else:
            print("%-40s 没找到白字行" % name)
            rows_out.append([name, "", "", "", "", "", 0, "", ""])
        if out_dir and vis is not None:
            base = os.path.splitext(name)[0]
            cv2.imwrite(os.path.join(out_dir, base + "_row.png"), vis)
    print("=" * 100)
    print("汇总: %d 张, 找到白字行 %d 张 (%.0f%%)" % (len(files), hit, 100.0 * hit / max(1, len(files))))
    if csv_path:
        with open(csv_path, "w", newline="", encoding="utf-8-sig") as fp:
            w = csv.writer(fp); w.writerow(["file", "x0", "y0", "x1", "y1", "ratio", "nchar", "row_fill", "blue_frac"])
            for r in rows_out: w.writerow(r)
        print("CSV ->", csv_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
