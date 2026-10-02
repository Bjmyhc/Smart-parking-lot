# -*- coding: utf-8 -*-
import io, sys
p = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\finetune_s3.py"
src = open(p, encoding="utf-8").read()
A1 = "    ap.add_argument('--freeze-until', type=int, default=0,"
N1 = ("    ap.add_argument('--val-dirs', default=None,\n"
      "                    help='验证目录(逗号分隔), 默认 data/official_val —— 那份 98%% 是皖, '\n"
      "                         '会把\"省字不塌\"的轮次判低分。P5.62 起建议 data/ccpd_plates/val (27 省均衡)')\n"
      + A1)
A2 = ("    val_dir = os.path.join(lprnet_dir, 'data', 'official_val')\n"
      "    va_ccpd = [p for p in collect([val_dir]) if all(c in CHARS_DICT for c in label_of(p))]")
N2 = ("    vd = (a.val_dirs or os.path.join(lprnet_dir, 'data', 'official_val')).split(',')\n"
      "    vd = [d if os.path.isabs(d) else os.path.join(lprnet_dir, d) for d in vd if d.strip()]\n"
      "    va_ccpd = [p for p in collect(vd) if all(c in CHARS_DICT for c in label_of(p))]")
assert src.count(A1) == 1, "A1 锚点不唯一"
assert src.count(A2) == 1, "A2 锚点不唯一"
src = src.replace(A1, N1).replace(A2, N2)
open(p, "w", encoding="utf-8", newline="\n").write(src)
import py_compile; py_compile.compile(p, doraise=True)
print("patched ok, bytes =", len(src))
