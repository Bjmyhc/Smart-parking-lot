# -*- coding: utf-8 -*-
import gzip, re, os

base = r'g:' + os.sep + '\u8d44\u6e90' + os.sep + '\u5355\u7247\u673a' + os.sep + 'ESP' + os.sep + 'ESP32S3-cam' + os.sep + '\u56fe\u4f20\u4f8b\u7a0b' + os.sep + 'CameraWebServer'
filepath = os.path.join(base, 'camera_index.h')

with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

pattern = r'const uint8_t (\w+)\[\] = \{(.*?)\};'
matches = re.findall(pattern, content, re.DOTALL)

print(f"Found {len(matches)} byte arrays")

for name, data_str in matches:
    hex_bytes = re.findall(r'0x([0-9a-fA-F]{2})', data_str)
    byte_array = bytes([int(b, 16) for b in hex_bytes])
    
    try:
        decompressed = gzip.decompress(byte_array)
        out_name = os.path.join(base, f"extracted_{name}.html")
        with open(out_name, 'wb') as f:
            f.write(decompressed)
        print(f"Extracted {name}: {len(byte_array)} bytes gzip -> {len(decompressed)} bytes HTML")
    except Exception as e:
        print(f"Failed to decompress {name}: {e}")

print("Done!")
