# -*- coding: utf-8 -*-
"""p561e_visual.py -- 生成 A/B 对照图: 左=现状(收左端) 右=关掉收边."""
import os, sys, glob, importlib.util
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
LPRNET = r"G:\All_Project\AI_Project\LPRNet_Pytorch"
sys.path.insert(0, os.path.join(ROOT, "tools")); sys.path.insert(0, LPRNET)
spec = importlib.util.spec_from_file_location("p561", os.path.join(ROOT, "tools", "p561_trim_ab.py"))
m = importlib.util.module_from_spec(spec); sys.modules["p561"] = m; spec.loader.exec_module(m)
import locator_replay as R
OUT = os.path.join(ROOT, "tools", "out", "p561"); os.makedirs(OUT, exist_ok=True)

PICKS = [
    ("\u4eacQ06666_\u96be", "IMG_20261001_165716_466714.jpg", True, 0.03, 0.12),
    ("\u4eacQ06666_\u96be", "IMG_20261001_165709_918281.jpg", True, 0.03, 0.12),
    ("\u8c6bFSQ818_\u96be", "IMG_20261001_194018_900684.jpg", True, 0.03, 0.12),
    ("\u8c6bA8F8Q8_\u96be", None, True, 0.03, 0.12),
]
def load(folder, name):
    if name is None:
        name = os.path.basename(sorted(glob.glob(os.path.join(m.BASE, folder, "*.jpg")))[0])
    return cv2.imdecode(np.fromfile(os.path.join(m.BASE, folder, name), np.uint8), cv2.IMREAD_COLOR), name

SC = 6
rows = []
for folder, name, _, pl, pv in PICKS:
    im, nm = load(folder, name)
    A = m.A; A.orig_w, A.orig_h = im.shape[1], im.shape[0]
    work, mask, info = R.locate(im.copy(), A)
    ch = info.get("chosen")
    if ch is None or ch.get("fit") is None:
        continue
    s = im.shape[1] / float(work.shape[1])
    qx = [v * s for v in ch["fit"]["qx"]]; qy = [v * s for v in ch["fit"]["qy"]]
    v_lo, v_hi = m.trim_short(im, qx, qy)
    uinfo = m.probe_u(im, qx, qy, v_lo, v_hi)
    c_lo = uinfo["c_lo"] if uinfo["c_lo"] >= 0 else 0.0
    u_tr = c_lo if uinfo["ok"] else 0.0
    m.PAD_L = 0.01; m.PAD_R = 0.08; m.PAD_V = 0.08
    b_tr = m.crop_quad(im, qx, qy, u_tr, 1.0, v_lo, v_hi)
    m.PAD_L = pl; m.PAD_R = 0.08; m.PAD_V = pv
    b_no = m.crop_quad(im, qx, qy, 0.0, 1.0, v_lo, v_hi)
    t_tr = m.infer94(b_tr); t_no = m.infer94(b_no)
    # 源图 + 两个取样框
    src = im.copy()
    def draw(u0, color, th):
        pts = []
        for (u, v) in ((u0, 0.0), (1.0, 0.0), (1.0, 1.0), (u0, 1.0)):
            x, y = m.quad_eval(qx, qy, u, v); pts.append([int(x), int(y)])
        cv2.polylines(src, [np.array(pts, np.int32)], True, color, th, cv2.LINE_AA)
    draw(0.0, (0, 0, 255), 1)
    draw(u_tr, (0, 255, 0), 1)
    src = cv2.resize(src, (src.shape[1] * 2, src.shape[0] * 2), interpolation=cv2.INTER_NEAREST)
    def up(b, txt, col):
        z = cv2.resize(b, (b.shape[1] * SC, b.shape[0] * SC), interpolation=cv2.INTER_NEAREST)
        z = cv2.copyMakeBorder(z, 22, 4, 4, 4, cv2.BORDER_CONSTANT, value=(255, 255, 255))
        cv2.putText(z, txt, (6, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45, col, 1, cv2.LINE_AA)
        return z
    a = up(b_tr, "with L-trim: %s" % t_tr.encode("ascii", "replace").decode(), (0, 0, 200))
    bb = up(b_no, "no trim   : %s" % t_no.encode("ascii", "replace").decode(), (0, 140, 0))
    h = max(a.shape[0], bb.shape[0], src.shape[0])
    def pad_h(z):
        if z.shape[0] < h:
            z = cv2.copyMakeBorder(z, 0, h - z.shape[0], 0, 0, cv2.BORDER_CONSTANT, value=(255, 255, 255))
        return z
    row = np.hstack([pad_h(src), pad_h(a), pad_h(bb)])
    rows.append(row)

if rows:
    w = max(r.shape[1] for r in rows)
    rows = [cv2.copyMakeBorder(r, 0, 0, 0, w - r.shape[1], cv2.BORDER_CONSTANT, value=(255, 255, 255)) for r in rows]
    img = np.vstack(rows)
    cv2.imwrite(os.path.join(OUT, "ab_visual.png"), img)
    print("wrote", os.path.join(OUT, "ab_visual.png"), img.shape)
