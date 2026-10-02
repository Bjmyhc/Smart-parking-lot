# -*- coding: utf-8 -*-
"""解析 18:23 「不同距离」串口日志: 逐帧对照 框大小 / 裁剪路径 / 蓝带 / 结果"""
import re, statistics as st

P = r"C:\Users\27678\.codex\attachments\ccb2f6a7-f2b4-42b9-bed4-562f17c293cd\Pasted text.txt"
lines = open(P, encoding="utf-8").read().splitlines()

RE_ROI = re.compile(r"#(\d+)\s+ROI\s+(\d+)x(\d+)\s+\u5360\u5c4f([\d.]+)%(.*?)\|\s*\u5b9a\u4f4d")
RE_TX  = re.compile(r"#(\d+)\s+\u91cd\u653e.*?->\s*(\S+)\s*\|\s*\u672c\u5e27\u5b9e\u65f6\s*->\s*(\S+)")
RE_RES = re.compile(r"#(\d+)\s+>>>\s+(?:RESULT:\s*(\S+)|\u7591\u4f3c\u8bef\u68c0.*?:\s*(\S+)).*?\u7f6e\u4fe1\s*(\d+)%")

frames = {}
order = []
for L in lines:
    m = RE_ROI.search(L)
    if m:
        n = int(m.group(1))
        if n not in frames: frames[n] = {}; order.append(n)
        f = frames[n]
        f["w"], f["h"] = int(m.group(2)), int(m.group(3))
        f["pct"] = float(m.group(4))
        tail = m.group(5)
        f["fb"] = "\u515c\u5e95" in tail
        f["ax"] = "\u8f74\u5bf9\u9f50" in tail
        mm = re.search(r"\u6536\u8fb9\[(\d+)%,(\d+)%\]", tail)      # 短边(上下)收边
        f["vt"] = (int(mm.group(1)), int(mm.group(2))) if mm else None
        mm = re.search(r"\u84dd\u5e26\[(\d+)%,(\d+)%\]", tail)      # 长边蓝带真正占的那段
        f["band"] = (int(mm.group(1)), int(mm.group(2))) if mm else None
        mm = re.search(r"\u5e73\u5747(\d+)x(\d+)\s*\u5f52\u4e00x([\d.]+)", L)
        if mm: f["ss"] = (int(mm.group(1)), int(mm.group(2)), float(mm.group(3)))
    m = RE_TX.search(L)
    if m:
        n = int(m.group(1))
        if n in frames:
            frames[n]["replay"], frames[n]["live"] = m.group(2), m.group(3)
    m = RE_RES.search(L)
    if m:
        n = int(m.group(1)); r = m.group(2) or m.group(3)
        if n in frames:
            frames[n]["res"] = r; frames[n]["conf"] = int(m.group(4))

GT = "\u4eacQ06666"
def cls(f):
    r = f.get("res")
    if not r: return "\u65e0\u7ed3\u679c"
    if r == GT: return "A \u5168\u5bf9"
    if r.startswith("\u4eac"): return "B \u7701\u5bf9\u6570\u5b57\u9519"
    return "C \u7701\u5b57\u9519(\u7696)"

print("===== \u9010\u5e27 =====")
print("%4s %9s %6s %-6s %-12s %-12s %-6s %-18s" % ("#","\u6846(px)","\u5360\u5c4f","\u8def\u5f84","\u77ed\u8fb9\u6536\u8fb9","\u84dd\u5e26(\u957f\u8fb9)","\u5f52\u4e00","\u7ed3\u679c"))
for n in order:
    f = frames[n]
    path = ("\u8f74\u5bf9\u9f50" if f.get("ax") else "\u6b63\u5e38") + ("+\u515c\u5e95" if f.get("fb") else "")
    band = "%d%%~%d%%" % f["band"] if f.get("band") else "-"
    vt = "%d%%~%d%%" % f["vt"] if f.get("vt") else "-"
    ss = "x%.2f" % f["ss"][2] if f.get("ss") else "-"
    print("%4d %4dx%-4d %5.1f%% %-6s %-12s %-12s %-6s %-18s %s" % (
        n, f["w"], f["h"], f["pct"], path, vt, band, ss,
        f.get("res","-"), cls(f)))

print()
print("===== \u5206\u7c7b\u6c47\u603b (40 \u5e27\u6709\u7ed3\u679c) =====")
from collections import Counter
c = Counter(cls(frames[n]) for n in order)
for k in sorted(c): print("  %-18s %2d \u5e27  %4.0f%%" % (k, c[k], 100.0*c[k]/sum(c.values())))
print("  \u5408\u8ba1 %d" % sum(c.values()))

print()
print("===== \u6309\u88c1\u526a\u8def\u5f84 =====")
for key, name in ((False,"\u6b63\u5e38\u6863"), (True,"\u515c\u5e95\u6863")):
    sub = [frames[n] for n in order if frames[n].get("fb") == key]
    full = sum(1 for f in sub if f.get("res") == GT)
    prov = sum(1 for f in sub if (f.get("res") or "").startswith("\u4eac"))
    print("  %-6s %2d \u5e27 | \u5168\u5bf9 %2d (%3.0f%%) | \u7701\u5b57\u5bf9 %2d (%3.0f%%)" % (
        name, len(sub), full, 100.0*full/max(len(sub),1), prov, 100.0*prov/max(len(sub),1)))

print()
print("===== \u84dd\u5e26\uff08\u6846\u5de6\u8fb9\u591a\u5403\u4e86\u591a\u5c11\u80cc\u666f\uff09=====")
for k in ("A \u5168\u5bf9","B \u7701\u5bf9\u6570\u5b57\u9519","C \u7701\u5b57\u9519(\u7696)"):
    v = [f["band"][0] for f in frames.values() if f.get("band") and cls(f) == k]
    v2 = [100 - f["band"][1] for f in frames.values() if f.get("band") and cls(f) == k]
    if v:
        print("  %-18s n=%2d | \u5de6\u7a7a\u767d \u4e2d\u4f4d %2d%% (\u6700\u5927 %2d%%) | \u53f3\u7a7a\u767d \u4e2d\u4f4d %2d%% (\u6700\u5927 %2d%%)" % (
            k, len(v), st.median(v), max(v), st.median(v2), max(v2)))

print()
print("===== \u91cd\u653e vs \u5b9e\u65f6\uff08\u786e\u5b9a\u6027\u81ea\u6821\uff09=====")
bad = [n for n in order if frames[n].get("replay") and frames[n].get("replay") != frames[n].get("live")]
print("  \u6709\u5bf9\u7167\u7684\u5e27: %d | \u4e0d\u4e00\u81f4: %d" % (
    sum(1 for n in order if frames[n].get("replay")), len(bad)))
