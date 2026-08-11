#!/usr/bin/env python3
"""
fw_pack.py - 给 APP .bin 拼 12 字节文件头 + 算 CRC32，生成升级用固件

用法:
  python fw_pack.py <input.bin> <版本号> [output.bin]

版本号格式: 主版本.次版本 (如 1.2 → 0x0102)
输出: 12B 文件头 + 原始 .bin

示例:
  python fw_pack.py Node/Output/app.bin 1.2 Tool/app_fw.bin
"""

import struct, zlib, sys, os

FW_MAGIC = 0xA55A

def pack_fw(input_path, version_str, output_path):
    # 原始 .bin
    with open(input_path, 'rb') as f:
        raw = f.read()

    # 解析版本号
    parts = version_str.split('.')
    ver = (int(parts[0]) << 8) | int(parts[1]) if len(parts) >= 2 else int(parts[0])

    # 12 字节文件头
    header = struct.pack('<HHII', FW_MAGIC, ver, len(raw), zlib.crc32(raw) & 0xFFFFFFFF)

    # 写入
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(raw)

    print(f"[OK] {input_path} -> {output_path}")
    print(f"     版本: 0x{ver:04X} ({version_str})")
    print(f"     代码长度: {len(raw)} 字节")
    print(f"     文件头 CRC32: 0x{zlib.crc32(raw) & 0xFFFFFFFF:08X}")
    print(f"     固件总大小: {len(header) + len(raw)} 字节")


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    in_path = sys.argv[1]
    version = sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else 'app_fw.bin'

    if not os.path.exists(in_path):
        print(f"[ERR] 找不到输入文件: {in_path}")
        sys.exit(1)

    pack_fw(in_path, version, out_path)