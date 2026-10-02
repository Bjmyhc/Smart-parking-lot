# -*- coding: utf-8 -*-
import io, os, sys, marshal, types, tempfile
sys.path.insert(0, r"C:\Users\27678\AppData\Local\Programs\Python\Python312\Lib\site-packages")
from PyInstaller.archive.readers import CArchiveReader, ZlibArchiveReader

EXE = r"G:\All_Project\AI_Project\BY串口助手\dist_new\BY串口助手.exe"
r = CArchiveReader(EXE)
names = [n for n in r.toc.keys() if n.upper().endswith(".PYZ") or "PYZ" in n.upper()]
print("archive entries with PYZ:", names[:5])
blob = r.extract(names[0])
tmp = os.path.join(tempfile.gettempdir(), "chk_pyz.pyz")
open(tmp, "wb").write(blob)
z = ZlibArchiveReader(tmp)
print("has serial_img_tool:", "serial_img_tool" in z.toc)
data = z.extract("serial_img_tool")
code = marshal.loads(data)

WANT = ["\u6536\u5e27\u8d85\u65f6", "\u56fe\u7247\u961f\u5217\u4e22\u5e27\u7d2f\u8ba1", "CRC \u6821\u9a8c\u5931\u8d25\u7d2f\u8ba1", "_img_drop_n", "frame_wd_resets"]
found = set()
def walk(c, depth=0):
    if depth > 12: return
    for k in getattr(c, "co_consts", ()) or ():
        if isinstance(k, str):
            for w in WANT:
                if w in k:
                    found.add(w)
        elif isinstance(k, types.CodeType):
            walk(k, depth + 1)
walk(code)
print("found:", sorted(found))
print("MISSING:", [w for w in WANT if w not in found])
