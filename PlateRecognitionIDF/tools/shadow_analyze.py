# -*- coding: utf-8 -*-
"""影子对照日志分析器 (P5.57/P5.58 固件)

用法:
    python shadow_analyze.py <串口日志.txt> [真值车牌, 默认 京Q06666]

判读影子那一行的三种结果:
    现状=... | 两端收=... | 只收左=...
    现状错 + 某个变体对  -> 那个变体就是解药
    [三者一致]           -> 这一帧对"长边收边"不敏感, 不构成证据
"""
import re, sys, io
from collections import Counter, defaultdict

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    path = sys.argv[1]
    truth = sys.argv[2] if len(sys.argv) > 2 else "\u4eacQ06666"

    txt = io.open(path, encoding="utf-8", errors="replace").read()
    lines = txt.splitlines()

    RE_SH = re.compile(
        r"#(\d+)\s+\u5f71\u5b50:\s*\u73b0\u72b6=(\S+)\s*\|\s*\u4e24\u7aef\u6536=(\S+)\s*\|\s*\u53ea\u6536\u5de6=(\S+)"
        r"(?:\s*\u84dd\u5e26\[(\d+)%~(\d+)%\])?(?:\s*\u591a\u5403\[\u5de6(\d+)% \u53f3(\d+)%\])?"
        r"(\s*\[\u4e09\u8005\u4e00\u81f4\])?")
    RE_ROI = re.compile(r"#(\d+)\s+ROI\s+(\d+)x(\d+)\s+\u5360\u5c4f([\d.]+)%")

    roi = {}
    sh = []
    for L in lines:
        m = RE_ROI.search(L)
        if m:
            roi[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), float(m.group(4)),
                                    "\u515c\u5e95" in L, "\u8f74\u5bf9\u9f50" in L)
        m = RE_SH.search(L)
        if m:
            sh.append(dict(n=int(m.group(1)), cur=m.group(2), both=m.group(3), left=m.group(4),
                           bl=int(m.group(5)) if m.group(5) else None,
                           br=int(m.group(6)) if m.group(6) else None,
                           exl=int(m.group(7)) if m.group(7) else None,
                           exr=int(m.group(8)) if m.group(8) else None,
                           same=bool(m.group(9))))

    if not sh:
        print("\u6ca1\u627e\u5230\u5f71\u5b50\u884c\u3002\u53ef\u80fd\u539f\u56e0: \u8fd9\u6b21\u6ca1\u62cd\u5230\u724c\u5b50 / "
              "\u84dd\u5e26\u6ca1\u91cf\u5230 / \u84dd\u5e26\u592a\u7a84(<60%%) \u88ab\u62a4\u680f\u8df3\u8fc7\u3002")
        return 1

    ok = lambda s: bool(s) and s.startswith(truth[:1]) and len(s) >= 6
    full = lambda s: s == truth

    print("\u771f\u503c = %s" % truth)
    print("\u5f71\u5b50\u5e27\u6570 = %d  (\u5176\u4e2d\u4e09\u8005\u4e00\u81f4 %d \u5e27, \u4e0d\u6784\u6210\u8bc1\u636e)" % (
        len(sh), sum(1 for r in sh if r["same"])))

    print("\n%-8s %6s %6s %6s   %s" % ("\u53d8\u4f53", "\u6574\u4e32\u5168\u5bf9", "\u7701\u5b57\u5bf9", "\u5408\u6cd5", "\u8bf4\u660e"))
    for key, name in (("cur", "\u73b0\u72b6"), ("both", "\u4e24\u7aef\u6536"), ("left", "\u53ea\u6536\u5de6")):
        v = [r[key] for r in sh]
        n = len(v)
        print("%-8s %5d(%3.0f%%) %5d(%3.0f%%) %5d(%3.0f%%)" % (
            name, sum(full(x) for x in v), 100.0*sum(full(x) for x in v)/n,
            sum(ok(x) for x in v), 100.0*sum(ok(x) for x in v)/n,
            sum(bool(x) for x in v), 100.0*sum(bool(x) for x in v)/n))

    print("\n\u2014\u2014 \u4e00\u5bf9\u4e00\u5934\u5bf9\u5934 (\u53ea\u770b\u4e0d\u4e00\u81f4\u7684\u5e27) \u2014\u2014")
    for a, b, na, nb in (("cur", "both", "\u73b0\u72b6", "\u4e24\u7aef\u6536"),
                         ("cur", "left", "\u73b0\u72b6", "\u53ea\u6536\u5de6"),
                         ("both", "left", "\u4e24\u7aef\u6536", "\u53ea\u6536\u5de6")):
        win = sum(1 for r in sh if full(r[b]) and not full(r[a]))
        lose = sum(1 for r in sh if full(r[a]) and not full(r[b]))
        print("  %-6s \u8d62 %-6s : %3d : %3d" % (nb, na, win, lose))

    print("\n\u2014\u2014 \u53ea\u6709\u73b0\u72b6\u9519\u3001\u53d8\u4f53\u5bf9\u7684\u5e27 (\u89e3\u836f\u5019\u9009) \u2014\u2014")
    hits = [r for r in sh if not full(r["cur"]) and (full(r["both"]) or full(r["left"]))]
    if not hits:
        print("  (\u6ca1\u6709)")
    for r in hits[:40]:
        w = roi.get(r["n"], (0, 0, 0, False, False))
        print("  #%-4d \u6846%4dx%-4d \u5360\u5c4f%5.1f%% \u591a\u5403[\u5de6%s%% \u53f3%s%%]  \u73b0\u72b6=%s \u4e24\u7aef\u6536=%s \u53ea\u6536\u5de6=%s" % (
            r["n"], w[0], w[1], w[2], r["exl"], r["exr"], r["cur"], r["both"], r["left"]))

    print("\n\u2014\u2014 \u53cd\u8fc7\u6765: \u73b0\u72b6\u5bf9\u3001\u53d8\u4f53\u9519\u7684\u5e27 \u2014\u2014")
    bad = [r for r in sh if full(r["cur"]) and not (full(r["both"]) and full(r["left"]))]
    if not bad:
        print("  (\u6ca1\u6709)")
    for r in bad[:40]:
        print("  #%-4d \u591a\u5403[\u5de6%s%% \u53f3%s%%]  \u73b0\u72b6=%s \u4e24\u7aef\u6536=%s \u53ea\u6536\u5de6=%s" % (
            r["n"], r["exl"], r["exr"], r["cur"], r["both"], r["left"]))

    print("\n\u2014\u2014 \u591a\u5403\u5206\u6863 vs \u73b0\u72b6\u5bf9\u9519 \u2014\u2014")
    buck = defaultdict(lambda: [0, 0])
    for r in sh:
        if r["exl"] is None: continue
        k = "\u5de6\u591a\u5403 >=3\u683c" if r["exl"] >= 9 else ("\u5de6\u591a\u5403 1~2\u683c" if r["exl"] > 0 else "\u6846\u5e72\u51c0")
        buck[k][0] += 1
        buck[k][1] += 1 if full(r["cur"]) else 0
    for k in ("\u6846\u5e72\u51c0", "\u5de6\u591a\u5403 1~2\u683c", "\u5de6\u591a\u5403 >=3\u683c"):
        if k in buck:
            n, g = buck[k]
            print("  %-16s n=%3d  \u73b0\u72b6\u5168\u5bf9 %3d (%3.0f%%)" % (k, n, g, 100.0*g/n))
    return 0

sys.exit(main())
