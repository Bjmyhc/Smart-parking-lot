# -*- coding: utf-8 -*-
"""检查单个源文件的编码（BOM / UTF-8 / GBK），GBK 时就地转成 UTF-8。

用法：
    python Tool/check_encoding.py <要检查的文件路径>

不传参数时默认检查仓库内的 PlateRecognition/PlateRecognition.ino。
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(_HERE)

if len(sys.argv) > 1:
    filepath = sys.argv[1]
else:
    filepath = os.path.join(_REPO_ROOT, 'PlateRecognition', 'PlateRecognition.ino')

if not os.path.exists(filepath):
    sys.exit('文件不存在: %s' % filepath)

with open(filepath, 'rb') as f:
    raw = f.read()

# Check BOM
if raw[:3] == b'\xef\xbb\xbf':
    print('BOM: UTF-8 BOM found')
    raw = raw[3:]
else:
    print('BOM: No BOM')

# Try decode
try:
    text = raw.decode('utf-8')
    print('Encoding: UTF-8 (valid)')
except:
    try:
        text = raw.decode('gbk')
        print('Encoding: GBK detected, converting to UTF-8...')
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(text)
        print('Converted to UTF-8')
    except:
        print('Encoding: Unknown')
