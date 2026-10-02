# -*- coding: utf-8 -*-
import re, collections
p = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p562\log2045.txt"
t = open(p, encoding="utf-8-sig", errors="replace").read()
rows = []
for l in t.splitlines():
    m = re.search(r"#(\d+).*?>>> (RESULT: (\S+)|疑似误检[^:]*: (\S+))", l)
    if m: rows.append((int(m.group(1)), m.group(3) or ("R:" + m.group(4))))
seen = {}
for f, s in rows: seen[f] = s
seq = [seen[k] for k in sorted(seen)]
c = collections.Counter(s[:1] for s in seq)
print("frames with result =", len(seq))
print("首字统计:", dict(c))
# 分界线: #1288 起是远的
near = [s for f, s in sorted(seen.items()) if f <= 1287]
far  = [s for f, s in sorted(seen.items()) if f >= 1288]
print("近(#1282-1287) n=%d:" % len(near), near)
print("远(#1288-1309) n=%d: 京开头 %d, 皖开头 %d" % (len(far), sum(1 for s in far if s[:1]=="京"), sum(1 for s in far if s[:1]=="皖")))
print("远的全部输出:", far)
