# -*- coding: utf-8 -*-
import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'
s = io.open(MAIN, encoding='utf-8', newline='').read()
pairs = [
 ('        bool probe_cmp_done = false;\n        std::string probe_cmp_txt;\n', '        std::string probe_cmp_txt;\n'),
 ('            probe_cmp_done = true;\n        }',
  '            ESP_LOGW(TAG, "#%d 同帧对照: 探针(已知能读对) -> %s | 本帧实时 -> %s",\n'
  '                     fn, probe_cmp_txt.c_str(), plate.c_str());\n        }'),
]
for i, (old, new) in enumerate(pairs):
    n = s.count(old)
    if n != 1:
        raise SystemExit('anchor %d matched %d times' % (i, n))
    s = s.replace(old, new)
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('patched')