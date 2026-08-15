# -*- coding: utf-8 -*-
import os

filepath = 'g:/All_Project/Keil_Project/STM32_Project/\u6211\u7684/\u667a\u80fd\u505c\u8f66\u573a/7dab3/PlateRecognition/PlateRecognition.ino'

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
