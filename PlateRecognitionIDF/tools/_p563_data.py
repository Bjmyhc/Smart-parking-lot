import os, glob, collections
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for d in [r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\collect2\京Q06666",
          r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\collect2\粤T666FP"]:
    fs = glob.glob(os.path.join(d, "*"))
    if fs:
        im = load(fs[0])
        print(os.path.basename(d), len(fs), "first:", os.path.basename(fs[0]), im.shape if im is not None else None)
for sub in ["train", "val"]:
    d = os.path.join(r"G:\All_Project\AI_Project\LPRNet_Pytorch\data\ccpd_plates", sub)
    c = collections.Counter()
    n = 0
    for f in os.listdir(d):
        c[f[0]] += 1; n += 1
        if n > 9000: break
    print("ccpd_plates/%-6s n=%d  provinces=%d  top: %s" % (sub, n, len(c), " ".join("%s%d"%(k,v) for k,v in c.most_common(6))))
