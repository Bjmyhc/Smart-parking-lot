# -*- coding: utf-8 -*-
"""_read_boot.py —— 复位开发板并抓 N 秒串口日志, 存成文件 (避免控制台把中文弄乱)。"""
# --- 可移植路径自举 (Tool/relocate_paths.py 注入, 勿手改) -------------------
import os as _os

_HERE_ = _os.path.dirname(_os.path.abspath(__file__))


def _find_repo_root_(start):
    """从 start 逐级上溯, 找到同时含 Tool/ 与 Node/ 的目录 = 仓库根。"""
    cur = start
    while True:
        if _os.path.isdir(_os.path.join(cur, 'Tool')) and \
           _os.path.isdir(_os.path.join(cur, 'Node')):
            return cur
        parent = _os.path.dirname(cur)
        if parent == cur:
            return _os.path.dirname(start)
        cur = parent


_REPO_ROOT_ = _find_repo_root_(_HERE_)
_AI_ROOT_ = _os.path.dirname(_REPO_ROOT_)
# --- 自举结束 ---------------------------------------------------------------

import sys, time, io, serial
port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
secs  = float(sys.argv[3]) if len(sys.argv) > 3 else 14.0
out   = sys.argv[4] if len(sys.argv) > 4 else _REPO_ROOT_ + r'\PlateRecognitionIDF\tools\out\boot_p560.log'
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
