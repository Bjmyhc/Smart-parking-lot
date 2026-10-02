# -*- coding: utf-8 -*-
p = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\finetune_s3.py"
src = open(p, encoding="utf-8").read()
A1 = "    best = (o1 / max(1, n1)) + (o2 / max(1, n2))\n    best_a = (o1 / max(1, n1), o2 / max(1, n2))"
N1 = ("    best = (o1 / max(1, n1)) + (o2 / max(1, n2))\n    best_a = (o1 / max(1, n1), o2 / max(1, n2))\n"
      "    if a.no_baseline_bar:\n"
      "        # P5.62: 基线本身在均衡 CCPD 上就有 75%+, 那道坎会把\"实拍大涨但 CCPD 略降\"的好轮次全拦掉。\n"
      "        #   这个开关让存盘只看\"本轮总分是否新高\", 不再要求超过微调前的基线。\n"
      "        best = -1.0")
A2 = "    ap.add_argument('--val-dirs', default=None,"
N2 = ("    ap.add_argument('--no-baseline-bar', action='store_true',\n"
      "                    help='存盘不要求超过微调前基线, 只取本轮最高分 (否则基线强的模型永远存不下东西)')\n"
      + A2)
assert src.count(A1) == 1 and src.count(A2) == 1, "锚点不唯一"
open(p, "w", encoding="utf-8", newline="\n").write(src.replace(A1, N1).replace(A2, N2))
import py_compile; py_compile.compile(p, doraise=True)
print("ok")
