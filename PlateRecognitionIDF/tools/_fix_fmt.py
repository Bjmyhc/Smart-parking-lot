import io
MAIN = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp'
s = io.open(MAIN, encoding='utf-8', newline='').read()
old = '), ck0, ck1,'
assert s.count(old) == 2, s.count(old)
s = s.replace(old, '), (unsigned)ck0, (unsigned)ck1,')
with io.open(MAIN, 'w', encoding='utf-8', newline='') as fh:
    fh.write(s)
print('fixed')