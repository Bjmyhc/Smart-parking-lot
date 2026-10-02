import re, io, os
p = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\p561\detail.txt"
rev = []; ok = []; nobox = 0
row = 0
for ln in io.open(p, encoding="utf-8"):
    ln = ln.rstrip("\n")
    if "  " not in ln and ln.strip() == "": continue
    m = re.search(r"\| u_ok=(\w) c_lo=([\d.]+) c_hi=([\d.]+) band=([\d.]+)", ln)
    if not m: 
        if "NO-BOX" in ln: nobox += 1
        continue
    row += 1
    parts = ln.split("|| ")[-1]
    d = {}
    for tok in parts.split("  "):
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    t = ln.split("\u771f\u503c ")[1].split(" |")[0]
    v0 = d.get("V0", "").rstrip("*"); v1 = d.get("V1", "").rstrip("*")
    if v0 == t and v1 != t: rev.append(ln)
    if v0 == t and v1 == t: ok.append(ln)
print("rows=%d nobox=%d  both_right=%d  V0right_V1wrong=%d" % (row, nobox, len(ok), len(rev)))
for ln in rev[:20]: print(ln)
