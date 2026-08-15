# -*- coding: utf-8 -*-
import gzip, re, os

base = r'g:' + os.sep + '\u8d44\u6e90' + os.sep + '\u5355\u7247\u673a' + os.sep + 'ESP' + os.sep + 'ESP32S3-cam' + os.sep + '\u56fe\u4f20\u4f8b\u7a0b' + os.sep + 'CameraWebServer'
html_file = os.path.join(base, 'extracted_index_ov2640_html_gz.html')
header_file = os.path.join(base, 'camera_index.h')

with open(html_file, 'r', encoding='utf-8') as f:
    html = f.read()

# Translations - Chinese values as unicode
T = {
    'Toggle OV2640 settings': '\u8bbe\u7f6e',
    'XCLK MHz': 'XCLK',
    'Resolution': '\u5206\u8fa8\u7387',
    'Quality': '\u8d28\u91cf',
    'Brightness': '\u4eae\u5ea6',
    'Contrast': '\u5bf9\u6bd4\u5ea6',
    'Saturation': '\u9971\u548c\u5ea6',
    'Special Effect': '\u7279\u6548',
    'No Effect': '\u65e0',
    'Negative': '\u8d1f\u7247',
    'Grayscale': '\u7070\u5ea6',
    'Red Tint': '\u7ea2\u8272',
    'Green Tint': '\u7eff\u8272',
    'Blue Tint': '\u84dd\u8272',
    'Sepia': '\u590d\u53e4',
    'AWB Gain': '\u767d\u5e73\u8861\u589e\u76ca',
    'WB Mode': '\u767d\u5e73\u8861\u6a21\u5f0f',
    'Auto': '\u81ea\u52a8',
    'Sunny': '\u6674\u5929',
    'Cloudy': '\u9634\u5929',
    'Office': '\u529e\u516c\u5ba4',
    'Home': '\u5ba4\u5185',
    'AEC SENSOR': '\u81ea\u52a8\u66dd\u514a\u4f20\u611f\u5668',
    'AEC DSP': '\u81ea\u52a8\u66dd\u514aDSP',
    'AE Level': '\u66dd\u514a\u7ea7\u522b',
    'Exposure': '\u66dd\u514a\u503c',
    'Gain': '\u589e\u76ca',
    'Gain Ceiling': '\u589e\u76ca\u4e0a\u9650',
    'BPC': '\u9ed1\u50cf\u7d20\u6821\u6b63',
    'WPC': '\u767d\u50cf\u7d20\u6821\u6b63',
    'Raw GMA': '\u539f\u59cb\u4f3d\u9a6c',
    'Lens Correction': '\u955c\u5934\u6821\u6b63',
    'H-Mirror': '\u6c34\u5e73\u955c\u50cf',
    'V-Flip': '\u5782\u76f4\u7ffb\u8f6c',
    'DCW (Downsize EN)': '\u964d\u91c7\u6837',
    'Color Bar': '\u989c\u8272\u6761',
    'LED Intensity': 'LED\u4eae\u5ea6',
    'Face Detection': '\u4eba\u8138\u68c0\u6d4b',
    'Face Recognition': '\u4eba\u8138\u8bc6\u522b',
    'Get Still': '\u62cd\u7167',
    'Start Stream': '\u5f00\u59cb\u89c6\u9891',
    'Enroll Face': '\u6ce8\u518c\u4eba\u8138',
    'Advanced Settings': '\u9ad8\u7ea7\u8bbe\u7f6e',
    'Register Get/Set': '\u5bc4\u5b58\u5668\u8bfb\u5199',
    'Reg, Mask, Value': '\u5bc4\u5b58\u5668,\u63a9\u7801,\u503c',
    'Reg, Mask': '\u5bc4\u5b58\u5668,\u63a9\u7801',
    'CLK 2X': '2\u500d\u65f6\u949f',
    'CLK DIV': '\u65f6\u949f\u5206\u9891',
    'Auto PCLK': '\u81ea\u52a8\u50cf\u7d20\u65f6\u949f',
    'PCLK DIV': '\u50cf\u7d20\u65f6\u949f\u5206\u9891',
    'Sensor Resolution': '\u4f20\u611f\u5668\u5206\u8fa8\u7387',
    'Offset': '\u504f\u79fb',
    'Window Size': '\u7a97\u53e3\u5927\u5c0f',
    'Output Size': '\u8f93\u51fa\u5927\u5c0f',
    'Set Resolution': '\u8bbe\u7f6e\u5206\u8fa8\u7387',
    'Save': '\u4fdd\u5b58',
    'Set': '\u8bbe\u7f6e',
    'Get': '\u83b7\u53d6',
    'Value': '\u503c',
}

# Apply translations - longer strings first
for en in sorted(T.keys(), key=len, reverse=True):
    html = html.replace(en, T[en])

# Fix title
html = html.replace('<title>ESP32 OV2460</title>', '<title>ESP32 \u6444\u50cf\u5934</title>')

# Fix error messages
html = html.replace("alert('Error['+code+']: '+txt)", "alert('\u9519\u8bef['+code+']: '+txt)")
html = html.replace("value.innerHTML = 'Error['+code+']: '+txt", "value.innerHTML = '\u9519\u8bef['+code+']: '+txt")

# Save translated HTML
out_html = os.path.join(base, 'index_ov2640.html')
with open(out_html, 'w', encoding='utf-8') as f:
    f.write(html)
print(f"Translated HTML: {len(html)} bytes")

# Compress with gzip
compressed = gzip.compress(html.encode('utf-8'), compresslevel=9)
print(f"Compressed: {len(compressed)} bytes")

# Generate new C array
lines = []
lines.append(f'//File: index_ov2640.html.gz, Size: {len(compressed)}')
lines.append(f'#define index_ov2640_html_gz_len {len(compressed)}')
lines.append('const uint8_t index_ov2640_html_gz[] = {')
for i in range(0, len(compressed), 16):
    chunk = compressed[i:i+16]
    line = ', '.join(f'0x{b:02X}' for b in chunk)
    if i + 16 < len(compressed):
        line += ','
    lines.append(f'  {line}')
lines.append('};')
lines.append('')
new_block = '\n'.join(lines)

# Update camera_index.h
with open(header_file, 'r', encoding='utf-8') as f:
    hdr = f.read()

# Find and replace OV2640 block
pattern = r'//File: index_ov2640\.html\.gz.*?\n\};'
match = re.search(pattern, hdr, re.DOTALL)
if match:
    print(f"Found OV2640 block at {match.start()}-{match.end()}")
    new_hdr = hdr[:match.start()] + new_block + hdr[match.end():]
else:
    # Try finding by array name
    pattern2 = r'//File: index_ov2640\.html\.gz.*?(?=//File: index_ov)'
    match2 = re.search(pattern2, hdr, re.DOTALL)
    if match2:
        end_pos = match2.end()
        # Find the closing };
        close_match = re.search(r'\n\};', hdr[match2.start():])
        if close_match:
            actual_end = match2.start() + close_match.end()
            print(f"Found OV2640 block at {match2.start()}-{actual_end}")
            new_hdr = hdr[:match2.start()] + new_block + hdr[actual_end:]
        else:
            print("ERROR: cannot find closing };")
            exit(1)
    else:
        print("ERROR: cannot find OV2640 block")
        exit(1)

with open(header_file, 'w', encoding='utf-8') as f:
    f.write(new_hdr)

print("Done! Recompile CameraWebServer.ino and flash.")
