# -*- coding: utf-8 -*-
"""_read_boot.py —— 复位开发板并抓 N 秒串口日志, 存成文件 (避免控制台把中文弄乱)。"""
import sys, time, io, serial
port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
secs  = float(sys.argv[3]) if len(sys.argv) > 3 else 14.0
out   = sys.argv[4] if len(sys.argv) > 4 else r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\tools\out\boot_p560.log"
s = serial.Serial(port, baud, timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.25); s.setRTS(False)
buf = bytearray(); t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(65536)
    if d:
        buf += d
s.close()
io.open(out, "wb").write(bytes(buf))
print("captured %d bytes -> %s" % (len(buf), out))
