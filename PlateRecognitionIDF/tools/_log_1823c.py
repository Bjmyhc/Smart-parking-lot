# -*- coding: utf-8 -*-
import re
P = r"C:\Users\27678\.codex\attachments\ccb2f6a7-f2b4-42b9-bed4-562f17c293cd\Pasted text.txt"
lines = open(P, encoding="utf-8").read().splitlines()
RE_ROI = re.compile(r"#(\d+)\s+ROI\s+(\d+)x(\d+)\s+\u5360\u5c4f([\d.]+)%(.*?)\|\s*\u5b9a\u4f4d")
RE_RES = re.compile(r"#(\d+)\s+>>>\s+(?:RESULT:\s*(\S+)|\u7591\u4f3c\u8bef\u68c0.*?:\s*(\S+)).*?\u7f6e\u4fe1\s*(\d+)%")
F = {}
for L in lines:
    m = RE_ROI.search(L)
    if m:
        n = int(m.group(1)); tail = m.group(5)
        mm = re.search(r"\u84dd\u5e26\[(\d+)%,(\d+)%\]", tail)
        F[n] = dict(w=int(m.group(2)), h=int(m.group(3)), pct=float(m.group(4)),
                    fb="\u515c\u5e95" in tail, ax="\u8f74\u5bf9\u9f50" in tail,
                    bl=int(mm.group(1)) if mm else None,
                    br=100-int(mm.group(2)) if mm else None)
    m = RE_RES.search(L)
    if m:
        n = int(m.group(1))
        if n in F:
            F[n]["res"] = m.group(2) or m.group(3); F[n]["conf"] = int(m.group(4))
GT = "\u4eacQ06666"
prov_ok = lambda r: bool(r) and r.startswith("\u4eac")
G = {n: f for n, f in F.items() if f["bl"] is not None}

print("\u84dd\u5e26\u91c7\u6837\u7cbe\u5ea6 = 1/24 \u2192 \u6bcf\u4e00\u683c\u5c31\u662f 4.2%\uff1b\u201c\u5de6 4%\u201d= \u6846\u5de6\u8fb9\u591a\u5403\u4e86 1 \u683c")
print()
hdr = "\u5de6\u591a\u5403 \u53f3\u591a\u5403 | \u5e27\u6570 \u5168\u5bf9 \u7701\u5b57\u5bf9"
print(hdr); print("-"*40)
for lo, ro in ((0,0), (4,0), (4,9), (9,9)):
    grp = [f for f in G.values() if f["bl"] >= lo and f["br"] >= ro]
    n = len(grp)
    if not n: continue
    full = sum(1 for f in grp if f.get("res") == GT)
    prov = sum(1 for f in grp if prov_ok(f.get("res")))
    print("\u2265%2d%%   \u2265%2d%%  | %3d  %3d(%3.0f%%) %3d(%3.0f%%)" % (lo, ro, n, full, 100.0*full/n, prov, 100.0*prov/n))
print()
print("\u2014\u2014 \u5806\u53e0\u67f1: \u6bcf\u4e2a\u6846\u7684\u201c\u591a\u5403\u201d\u5206\u6863 vs \u7ed3\u679c \u2014\u2014")
import collections
buckets = collections.OrderedDict()
for name, fn in (("\u5e72\u51c0 (\u5de6=0 \u4e14 \u53f3=0)", lambda f: f["bl"] == 0 and f["br"] == 0),
                 ("\u5c11\u5403 (1~2 \u683c)",        lambda f: max(f["bl"], f["br"]) // 4 in (1, 2) and f["bl"] < 9 and f["br"] < 9),
                 ("\u591a\u5403 (\u22653 \u683c)",       lambda f: f["bl"] >= 9 or f["br"] >= 9)):
    grp = [f for f in G.values() if fn(f)]
    n = len(grp); bad = sum(1 for f in grp if not prov_ok(f.get("res")))
    print("  %-22s n=%2d  \u7701\u5b57\u9519 %2d (%3.0f%%)" % (name, n, bad, 100.0*bad/max(n,1)))
print()
print("\u2014\u2014 \u5206\u7c7b\u6c47\u603b\u2014\u2014")
for k, fn in (("\u5168\u5bf9", lambda r: r == GT), ("\u7701\u5bf9\u6570\u5b57\u9519", lambda r: r and r.startswith("\u4eac") and r != GT),
              ("\u7701\u5b57\u9519", lambda r: r and not r.startswith("\u4eac"))):
    grp = [f for f in F.values() if fn(f.get("res"))]
    print("  %-10s %2d \u5e27" % (k, len(grp)))
