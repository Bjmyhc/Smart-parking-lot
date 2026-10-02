import sys, os, struct, hashlib, glob
ROOT = r'G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF'
blob = open(sys.argv[1],'rb').read()
out_dir = sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
i = 0; n = 0
while True:
    j = blob.find(b'EDL2', i)
    if j < 0: break
    mode, size, pad = struct.unpack('<III', blob[j+4:j+16])
    if 100000 < size < 4000000 and j + 16 + size <= len(blob):
        data = blob[j:j+16+size]
        out = os.path.join(out_dir, 'flash_model_%d_%d.espdl' % (j, size))
        open(out,'wb').write(data)
        print('提取 %s  size=%d sha1=%s' % (out, size, hashlib.sha1(data).hexdigest()[:16]))
        n += 1
    i = j + 1
print('共提取 %d 个候选' % n)
print('')
print('%-58s %9s %s' % ('已有 espdl 文件', 'size', 'sha1(前16)'))
for p in sorted(glob.glob(os.path.join(ROOT, '**', '*.espdl'), recursive=True)):
    d = open(p,'rb').read()
    rel = os.path.relpath(p, ROOT)
    print('%-58s %9d %s' % (rel, len(d), hashlib.sha1(d).hexdigest()[:16]))