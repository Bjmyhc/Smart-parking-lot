import sys, os
p = sys.argv[1]
blob = open(p,'rb').read()
print('bin size %d (%.1f MB)' % (len(blob), len(blob)/1048576.0))
idx = []
i = 0
while True:
    j = blob.find(b'EDL2', i)
    if j < 0: break
    idx.append(j); i = j + 1
print('EDL2 出现次数:', len(idx))
import struct
for j in idx[:10]:
    mode, size, pad = struct.unpack('<III', blob[j+4:j+16])
    print('  @0x%x mode=%d size=%d pad=%d  -> 0x%x..0x%x' % (j, mode, size, pad, j, j+16+size))