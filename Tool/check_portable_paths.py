# -*- coding: utf-8 -*-
"""校验：确认仓库内已无写死的旧盘符绝对路径，且核心脚本的默认路径已指向当前仓库。

用法：
    python Tool/check_portable_paths.py

退出码 0 = 干净（仓库搬家后无需任何改动）；1 = 还有残留，逐行列出。

注意：Tool/relocate_paths.py 里的旧路径是"搜索模式"本身，属合法数据，这里跳过。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {"venv", "build", "_gw_out", ".git", "managed_components", ".cache", "out"}
SKIP_FILES = {os.path.join(ROOT, "Tool", "relocate_paths.py")}
BAD = re.compile(r"[Gg]:[\\/]+All_Project")

hits = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
    for name in filenames:
        if not name.endswith((".py", ".ps1", ".bat")):
            continue
        path = os.path.join(dirpath, name)
        if path in SKIP_FILES:
            continue
        try:
            text = open(path, encoding="utf-8-sig", errors="ignore").read()
        except OSError:
            continue
        for no, line in enumerate(text.splitlines(), 1):
            if BAD.search(line):
                hits.append("%s:%d: %s" % (os.path.relpath(path, ROOT), no, line.strip()[:140]))

print("[1] 残留写死 G: 路径的可执行脚本行数 = %d" % len(hits))
for h in hits:
    print("    " + h)

print("[2] 核心脚本默认路径")
for rel in (
    "PlateRecognitionIDF/tools/export_onnx_s3.py",
    "PlateRecognitionIDF/tools/quant_espdl_s3.py",
    "PlateRecognitionIDF/tools/inspect_espdl.py",
    "analysis_out/eval_crops.py",
    "PlateRecognitionIDF/tools/diag_crops.py",
):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        print("    %-46s <缺失>" % rel)
        continue
    src = open(path, encoding="utf-8-sig", errors="ignore").read()
    found = False
    for m in re.finditer(r"^(\w+)\s*=\s*(.+)$", src, re.M):
        name, val = m.group(1), m.group(2)
        if name.isupper() and ("ROOT" in name or "DIR" in name or "SITE" in name or name == "HERE"):
            print("    %-46s %s = %s" % (rel, name, val[:110]))
            found = True
    if not found:
        print("    %-46s <无顶层大写路径常量>" % rel)

sys.exit(1 if hits else 0)
