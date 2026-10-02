import os, sys, glob
import numpy as np, cv2
ROOT = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
sys.path.insert(0, os.path.join(ROOT, "tools"))
import locator_replay as R
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
class A: pass
a=A(); a.grid_step=None; a.s_blue=R.S_BLUE_DEF; a.s_green=R.S_GREEN_DEF; a.h_lo=R.H_LO; a.h_hi=R.H_HI
a.orig_w=320; a.orig_h=240; a.min_area_ratio=R.MIN_AREA_RATIO; a.min_fill=R.MIN_FILL
a.ratio_lo=R.RATIO_LO; a.ratio_hi=R.RATIO_HI; a.fit_min_fill=R.FIT_MIN_FILL
a.max_area_pct=0.0; a.no_fallback=False; a.no_edge_skip=False; a.no_image=True
a.keep_green_box=False; a.path=""; a.out_dir=None; a.csv=None; a.sweep_s=None; a.v_min=120.0
R.V_MIN_CUR=120.0
p = sorted(glob.glob(r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images\京Q06666_难\*"))[0]
im = load(p)
work, mask, info = R.locate(im, a)
ch = info.get("chosen")
print("chosen is None?", ch is None)
if ch:
    print("fit keys:", list((ch.get('fit') or {}).keys()))
    for k, v in (ch.get('fit') or {}).items():
        print("   ", k, "=", str(v)[:120])
    print("step:", info.get("step"), "work_size:", info.get("work_size"), "src:", info.get("src_size"))
