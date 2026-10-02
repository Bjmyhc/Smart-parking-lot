# -*- coding: utf-8 -*-
"""p561_trim_ab.py -- A/B: P5.59 的"左端长边收边"到底在帮忙, 还是在切掉省字.

复算口径 = 固件 P5.60 的真实采样链:
  locate(b-r 判据 / 比例2.2~4.2 / 填充>=0.60 / fit>=0.35 / 面积<=45% / 贴边照用)
  -> 选中的四边形 qx,qy (已含 fit 的 TRIM_X 去边)
  -> roi_trim_short_axis      (短边 v 收边)
  -> roi_probe_utrim          (长边 u, 只收左端, P5.59)
  -> sample_quad_to_input     (94x24, 默认档2 留白: 左1% 右8% 上下8%)
  -> 整数饱和 x2.0 -> r4 浮点模型 -> CTC 解码

变体:
  V0 = 固件现状   (u 收左端, 规则照抄 P5.59)
  V1 = 完全不收 u (u_lo = 0)
  V2 = 只在"收完比例仍 >= 2.9"时才收 (真车牌长宽比约 3.1~3.4)
  V3 = 只收掉很窄的一条 (c_lo <= 0.12)
"""
import os, sys, glob, argparse
import numpy as np, cv2, torch

ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
import locator_replay as R
from data.load_data import CHARS
from lprnet_s3_model import build_lprnet_s3

BASE = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
SETS = [("\u4eacQ06666_\u96be", "\u4eacQ06666"),
        ("\u8c6bFSQ818_\u96be", "\u8c6bFSQ818"),
        ("\u8c6bA8F8Q8_\u96be", "\u8c6bA8F8Q8")]
OUT = os.path.join(ROOT, "tools", "out", "p561"); os.makedirs(OUT, exist_ok=True)

IMG_W, IMG_H = 94, 24
PAD_L, PAD_R, PAD_V = 0.01, 0.08, 0.08
NSEG, NPT, NEED = 24, 9, 4
V_TRIM_MAX, V_TRIM_KEEP = 0.25, 0.60
U_TRIM_LEFT_MAX, U_TRIM_BAND_MIN = 0.45, 0.60

blank = len(CHARS) - 1

def dec(out):
    a = np.squeeze(np.asarray(out, dtype=np.float32))
    if a.shape[0] != len(CHARS):
        a = a.T
    lab = [int(np.argmax(a[:, j])) for j in range(a.shape[1])]
    res, prev = [], blank
    for c in lab:
        if c != prev and c != blank:
            res.append(c)
        prev = c
    return "".join(CHARS[i] for i in res)

torch.set_grad_enabled(False)
net = build_lprnet_s3(lpr_max_len=8, class_num=len(CHARS), dropout_rate=0)
net.load_state_dict(torch.load(os.path.join(ROOT, "tools", "out", "finetune", "r4_lr1e4_nofreeze.pth"),
                               map_location="cpu"))
net.eval()

def infer94(blk):
    if blk is None or blk.size == 0:
        return ""
    im = (blk.astype(np.float32) - 127.5) * 0.0078125
    return dec(net(torch.from_numpy(im.transpose(2, 0, 1)[None])).numpy()[0])

def rgb_sat_int(img, k256=512):
    f = img.astype(np.int32); v = f.max(axis=2, keepdims=True)
    return np.clip(v + ((f - v) * k256) // 256, 0, 255).astype(np.uint8)

# ---------------- 固件那套颜色判据 ----------------
def is_plate_px(p):
    b, g, r = float(p[0]), float(p[1]), float(p[2])
    v = max(r, g, b)
    if v < 120.0:
        return False
    return (b - r) > 40.0 and b > 120.0

def quad_eval(qx, qy, u, v):
    w0 = (1 - u) * (1 - v); w1 = u * (1 - v); w2 = u * v; w3 = (1 - u) * v
    return (w0 * qx[0] + w1 * qx[1] + w2 * qx[2] + w3 * qx[3],
            w0 * qy[0] + w1 * qy[1] + w2 * qy[2] + w3 * qy[3])

def sample_px(im, x, y):
    H, W = im.shape[:2]
    x = min(max(x, 0.0), W - 1.0); y = min(max(y, 0.0), H - 1.0)
    x0 = int(x); y0 = int(y); x1 = min(x0 + 1, W - 1); y1 = min(y0 + 1, H - 1)
    fx = x - x0; fy = y - y0
    return (im[y0, x0].astype(np.float64) * (1 - fx) * (1 - fy) + im[y0, x1] * fx * (1 - fy)
            + im[y1, x0] * (1 - fx) * fy + im[y1, x1] * fx * fy)

def trim_short(im, qx, qy):
    cov = []
    for j in range(NSEG):
        v = (j + 0.5) / float(NSEG)
        hit = 0
        for i in range(NPT):
            u = (i + 1.0) / (NPT + 1.0)
            x, y = quad_eval(qx, qy, u, v)
            if is_plate_px(sample_px(im, x, y)):
                hit += 1
        cov.append(hit)
    jt = jb = -1
    for j in range(NSEG - 2):
        if cov[j] >= NEED and cov[j + 1] >= NEED and cov[j + 2] >= NEED:
            jt = j; break
    for j in range(NSEG - 1, 1, -1):
        if cov[j] >= NEED and cov[j - 1] >= NEED and cov[j - 2] >= NEED:
            jb = j; break
    if jt < 0 or jb < 0:
        return 0.0, 1.0
    lo = jt / float(NSEG); hi = (jb + 1) / float(NSEG)
    if lo > V_TRIM_MAX: lo = 0.0
    if 1.0 - hi > V_TRIM_MAX: hi = 1.0
    if hi - lo < V_TRIM_KEEP:
        return 0.0, 1.0
    return lo, hi

def probe_u(im, qx, qy, v_lo, v_hi):
    cov = []
    for j in range(NSEG):
        u = (j + 0.5) / float(NSEG)
        hit = 0
        for i in range(NPT):
            v = v_lo + (i + 1.0) / (NPT + 1.0) * (v_hi - v_lo)
            x, y = quad_eval(qx, qy, u, v)
            if is_plate_px(sample_px(im, x, y)):
                hit += 1
        cov.append(hit)
    jl = jr = -1
    for j in range(NSEG - 2):
        if cov[j] >= NEED and cov[j + 1] >= NEED and cov[j + 2] >= NEED:
            jl = j; break
    for j in range(NSEG - 1, 1, -1):
        if cov[j] >= NEED and cov[j - 1] >= NEED and cov[j - 2] >= NEED:
            jr = j; break
    info = {"cov": cov, "c_lo": -1.0, "c_hi": -1.0, "band": -1.0, "ok": False}
    if jl < 0 or jr <= jl:
        return info
    c_lo = jl / float(NSEG); c_hi = (jr + 1) / float(NSEG)
    info["c_lo"] = c_lo; info["c_hi"] = c_hi; info["band"] = c_hi - c_lo
    info["ok"] = (c_hi - c_lo) >= U_TRIM_BAND_MIN and c_lo <= U_TRIM_LEFT_MAX
    return info

def crop_quad(im, qx, qy, u_lo, u_hi, v_lo, v_hi):
    span_v = v_hi - v_lo
    span_u_raw = u_hi - u_lo
    span_u = span_u_raw * (1.0 + PAD_L + PAD_R)
    u_base = u_lo - PAD_L * span_u_raw
    v_base = v_lo - PAD_V * span_v
    du = span_u / float(IMG_W)
    dv = (span_v * (1.0 + 2.0 * PAD_V)) / float(IMG_H)
    xs = (np.arange(IMG_W)[None, :] + 0.5) * du + u_base
    ys = (np.arange(IMG_H)[:, None] + 0.5) * dv + v_base
    U = np.broadcast_to(xs, (IMG_H, IMG_W)); V = np.broadcast_to(ys, (IMG_H, IMG_W))
    w0 = (1 - U) * (1 - V); w1 = U * (1 - V); w2 = U * V; w3 = (1 - U) * V
    mx = (w0 * qx[0] + w1 * qx[1] + w2 * qx[2] + w3 * qx[3]).astype(np.float32)
    my = (w0 * qy[0] + w1 * qy[1] + w2 * qy[2] + w3 * qy[3]).astype(np.float32)
    blk = cv2.remap(im, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    if blk.shape[0] != IMG_H or blk.shape[1] != IMG_W:
        blk = cv2.resize(blk, (IMG_W, IMG_H))
    return rgb_sat_int(blk, 512)

def mask_br(bgr, s_blue, s_green, h_lo, h_hi):
    b = bgr[:, :, 0].astype(np.int32); r = bgr[:, :, 2].astype(np.int32)
    return ((b - r) > 40) & (b > 120)

R.plate_mask = mask_br
R.V_MIN_CUR = 120
R.S_BLUE_MIN = 35
A = argparse.Namespace(grid_step=None, s_blue=35.0, s_green=43.0, h_lo=180.0, h_hi=270.0,
                       min_area_ratio=0.02, ratio_lo=2.2, ratio_hi=4.2, min_fill=0.60,
                       fit_min_fill=0.35, max_area_pct=45.0, no_fallback=False,
                       no_edge_skip=True, keep_green_box=False, orig_w=320, orig_h=240)

VARIANTS = ["V0_\u6536\u5de6\u7aef(\u73b0\u72b6)", "V1_\u4e0d\u6536", "V2_\u6bd4\u4f8b\u62a4\u680f>=2.9", "V3_\u53ea\u6536<=12%"]
res = {k: {"hit": 0, "ok": 0} for k in VARIANTS}
lines = []
applied_cnt = 0
smoke = []   # 现状错/不收对 的帧
for folder, truth in SETS:
    files = sorted(glob.glob(os.path.join(BASE, folder, "*.jpg")))
    for f in files:
        im = cv2.imdecode(np.fromfile(f, np.uint8), cv2.IMREAD_COLOR)
        if im is None or im.shape[0] != 240:
            continue
        A.orig_w, A.orig_h = im.shape[1], im.shape[0]
        work, mask, info = R.locate(im.copy(), A)
        ch = info.get("chosen")
        name = os.path.basename(f)
        for k in VARIANTS:
            res[k]["hit"] += 1
        if ch is None:
            lines.append("%s/%s  NO-BOX  %s" % (folder, name, info.get("verdict")))
            continue
        fit = ch.get("fit")
        if fit is None:
            lines.append("%s/%s  NO-QUAD" % (folder, name))
            continue
        s = im.shape[1] / float(work.shape[1])
        qx = [v * s for v in fit["qx"]]; qy = [v * s for v in fit["qy"]]
        v_lo, v_hi = trim_short(im, qx, qy)
        uinfo = probe_u(im, qx, qy, v_lo, v_hi)
        c_lo = uinfo["c_lo"] if uinfo["c_lo"] >= 0 else 0.0
        ulen = float(np.hypot(qx[1] - qx[0], qy[1] - qy[0]))
        vlen = float(np.hypot(qx[3] - qx[0], qy[3] - qy[0])) * (v_hi - v_lo)
        ratio_new = (ulen * (1.0 - c_lo) / vlen) if vlen > 1.0 else 0.0
        if uinfo["ok"]:
            applied_cnt += 1
        u_lo_by = {
            "V0_\u6536\u5de6\u7aef(\u73b0\u72b6)": c_lo if uinfo["ok"] else 0.0,
            "V1_\u4e0d\u6536": 0.0,
            "V2_\u6bd4\u4f8b\u62a4\u680f>=2.9": c_lo if (uinfo["ok"] and ratio_new >= 2.9) else 0.0,
            "V3_\u53ea\u6536<=12%": c_lo if (uinfo["ok"] and c_lo <= 0.12) else 0.0,
        }
        got = {}
        for k in VARIANTS:
            blk = crop_quad(im, qx, qy, u_lo_by[k], 1.0, v_lo, v_hi)
            t = infer94(blk)
            got[k] = t
            res[k]["ok"] += (t == truth)
        lines.append("%s/%s  真值 %s | u_ok=%s c_lo=%.2f c_hi=%.2f band=%.2f v=[%.2f,%.2f] ratio_new=%.2f || %s" % (
            folder, name, truth, "T" if uinfo["ok"] else "F", c_lo, uinfo["c_hi"], uinfo["band"],
            v_lo, v_hi, ratio_new,
            "  ".join("%s=%s%s" % (k.split("_")[0], got[k], "*" if got[k] == truth else "") for k in VARIANTS)))
        if got["V0_\u6536\u5de6\u7aef(\u73b0\u72b6)"] != truth and got["V1_\u4e0d\u6536"] == truth:
            smoke.append("%s/%s  c_lo=%.2f band=%.2f  %s -> V0=%s V1=%s" % (
                folder, name, c_lo, uinfo["band"], truth, got["V0_\u6536\u5de6\u7aef(\u73b0\u72b6)"], got["V1_\u4e0d\u6536"]))

with open(os.path.join(OUT, "detail.txt"), "w", encoding="utf-8") as fp:
    fp.write("\n".join(lines))
with open(os.path.join(OUT, "summary.txt"), "w", encoding="utf-8") as fp:
    fp.write("frames=%d  u-trim applied=%d\n" % (res[VARIANTS[0]]["hit"], applied_cnt))
    for k in VARIANTS:
        n = res[k]["hit"]; o = res[k]["ok"]
        fp.write("%-28s 整串全对 %3d/%3d (%3.0f%%)\n" % (k, o, n, 100.0 * o / max(1, n)))
    fp.write("\n[现状错 / 不收对] 共 %d 帧:\n" % len(smoke))
    fp.write("\n".join(smoke))
print("frames=%d applied=%d" % (res[VARIANTS[0]]["hit"], applied_cnt))
for k in VARIANTS:
    print("%-28s %3d/%3d %3.0f%%" % (k.encode("ascii", "replace").decode(), res[k]["ok"], res[k]["hit"],
                                     100.0 * res[k]["ok"] / max(1, res[k]["hit"])))
print("smoke=%d" % len(smoke))
print("out=" + OUT)
