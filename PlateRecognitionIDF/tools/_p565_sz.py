import glob, os
import numpy as np, cv2
def load(p): return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
for f in ["京Q06666_难","豫FSQ818_难","豫A8F8Q8_难"]:
    fs = sorted(glob.glob(os.path.join(r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images", f, "*")))
    print(f, len(fs), os.path.basename(fs[0]), load(fs[0]).shape)
