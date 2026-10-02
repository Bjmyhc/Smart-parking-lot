import glob, os
import numpy as np, cv2
d = r"G:\All_Project\AI_Project\BY串口助手\dist\saved\images"
fs = sorted(glob.glob(os.path.join(d, "IMG_20261001_2045*.jpg")))
for p in fs:
    im = cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)
    print(os.path.basename(p), im.shape, "%.1f KB" % (os.path.getsize(p)/1024))
