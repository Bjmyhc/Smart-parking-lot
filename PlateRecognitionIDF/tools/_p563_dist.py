import os, collections, glob
L = r"G:\All_Project\AI_Project\LPRNet_Pytorch\data"
for sub in ["official_train", "official_val", "ccpd_plates"]:
    d = os.path.join(L, sub)
    if not os.path.isdir(d):
        print(sub, "-> missing"); continue
    fs = []
    for root, dirs, files in os.walk(d):
        for f in files:
            if f.lower().endswith((".jpg",".png",".jpeg")): fs.append(f)
        if len(fs) > 40000: break
    c = collections.Counter(f[0] for f in fs if f)
    print("%-14s n=%6d  top10: %s" % (sub, len(fs), " ".join("%s%d" % (k,v) for k,v in c.most_common(10))))
print()
d2 = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\collect2"
for root, dirs, files in os.walk(d2):
    print(root, "->", len([f for f in files if f.lower().endswith(('.jpg','.png'))]))
