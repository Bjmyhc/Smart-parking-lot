p = r"C:\Users\27678\.codex\attachments\8537ce1c-e3e1-48e9-87af-4387d38bf7c6\Pasted text.txt"
raw = open(p, "rb").read()
for enc in ("utf-8","utf-16","gbk"):
    try:
        t = raw.decode(enc); break
    except Exception: pass
lines = t.splitlines()
print("lines =", len(lines), "bytes =", len(raw))
keep = [l for l in lines if "plate:" in l]
print("plate lines =", len(keep))
print("\n".join(keep))
