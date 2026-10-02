# -*- coding: utf-8 -*-
import re
P = r"C:\Users\27678\.codex\attachments\ccb2f6a7-f2b4-42b9-bed4-562f17c293cd\Pasted text.txt"
lines = open(P, encoding="utf-8").read().splitlines()
RE_ROI = re.compile(r"#(\d+)\s+ROI\s+(\d+)x(\d+)\s+\u5360\u5c4f([\d.]+)%(.*?)\|\s*\u5b9a\u4f4d")
RE_RES = re.compile(r"#(\d+)\s+>>>\s+(?:RESULT:\s*(\S+)|\u7591\u4f3c\u8bef\u68c0.*?:\s*(\S+))")
F = {}
for L in lines:
    m = RE_ROI.search(L)
    if m:
        n = int(m.group(1)); tail = m.group(5)
        mm = re.search(r"\u84dd\u5e26\[(\d+)%,(\d+)%\]", tail)
        F[n] = dict(w=int(m.group(2)), pct=float(m.group(4)),
                    fb="\u515c\u5e95" in tail,
                    bl=int(mm.group(1)) if mm else None,
                    br=100-int(mm.group(2)) if mm else None)
    m = RE_RES.search(L)
    if m:
        n = int(m.group(1))
        if n in F: F[n]["res"] = m.group(2) or m.group(3)
GT = "\u4eacQ06666"
for n, f in F.items():
    f["ok"] = (f.get("res") == GT)

# 只看有蓝带数据的帧
G = {n: f for n, f in F.items() if f["bl"] is not None}
print("\u6709\u84dd\u5e26\u6570\u636e\u7684\u5e27: %d" % len(G))
print()
for thr in (4, 6, 8, 10, 12):
    hit = [f for f in G.values() if f["bl"] > thr or f["br"] > 4]
    hit_bad = [f for f in hit if not f["ok"]]
    clr = [f for f in G.values() if not (f["bl"] > thr or f["br"] > 4)]
    clr_bad = [f for f in clr if not f["ok"]]
    print("\u9608\u503c \u5de6>%2d%% \u6216 \u53f3>4%% : \u547d\u4e2d %2d \u5e27, \u5176\u4e2d\u9519 %2d (%.0f%%) | \u4f59\u4e0b %2d \u5e27, \u9519 %2d (%.0f%%)" % (
        thr, len(hit), len(hit_bad), 100.0*len(hit_bad)/max(len(hit),1),
        len(clr), len(clr_bad), 100.0*len(clr_bad)/max(len(clr),1)))
print()
print("\u2014\u2014 \u6846\u5403\u5f97\u5c11\u4f46\u4ecd\u9519\u7684\u5e27 (\u5de6<=6%% \u4e14 \u53f3<=4%%) \u2014\u2014")
for n in sorted(G):
    f = G[n]
    if not f["ok"] and f["bl"] <= 6 and f["br"] <= 4:
        print("   #%-4d \u6846%4dpx \u5360\u5c4f%5.1f%% \u84dd\u5e26\u5de6%2d%% \u53f3%2d%% %s%s -> %s" % (
            n, f["w"], f["pct"], f["bl"], f["br"], "\u515c\u5e95" if f["fb"] else "\u6b63\u5e38", "", f.get("res")))
print()
print("\u2014\u2014 \u6846\u5403\u5f97\u591a\u4f46\u5bf9\u4e86\u7684\u5e27 \u2014\u2014")
for n in sorted(G):
    f = G[n]
    if f["ok"] and (f["bl"] > 6 or f["br"] > 4):
        print("   #%-4d \u6846%4dpx \u84dd\u5e26\u5de6%2d%% \u53f3%2d%% %s -> %s" % (
            n, f["w"], f["bl"], f["br"], "\u515c\u5e95" if f["fb"] else "\u6b63\u5e38", f.get("res")))
