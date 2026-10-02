# -*- coding: utf-8 -*-
"""_sniff.py -- 串口取数照妖镜: 复刻 BY串口助手的解析状态机, 逐块打时间戳。

用法:  python _sniff.py COM10 921600 60 out_prefix
输出:  <prefix>_chunks.txt   每块到达时刻 + 字节数
       <prefix>_report.txt   汇总 (块数/最大空档/每秒字节/帧数/CRC/失步)
判定:  设备停发 -> 出现秒级空档; 助手卡住 -> 流一直很密但助手不显示
"""
import sys, os, time, struct, binascii, collections

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
    dur = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
    pref = sys.argv[4] if len(sys.argv) > 4 else "sniff"
    import serial
    ser = serial.Serial(port, baud, timeout=0.05)
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.25); ser.setRTS(False)   # 复位 -> 从开机日志开始
    time.sleep(0.1)

    t0 = time.time()
    state = "WAIT_HEADER"
    line_buf = bytearray(); bin_buf = bytearray(); crc_buf = bytearray(); end_buf = bytearray()
    bin_expected = 0
    n_frame = 0; n_crc_bad = 0; n_crc_ok = 0; n_logline = 0
    sizes = []
    desync = 0            # WAIT_HEADER 里收到过"不像日志"的字节(高字节/控制字符)
    chunks = []
    per_sec = collections.Counter()
    last_t = t0
    max_gap = (0.0, 0.0)

    while time.time() - t0 < dur:
        n = ser.in_waiting
        if n == 0:
            b = ser.read(1)
            if not b:
                continue
            chunk = b
        else:
            chunk = ser.read(min(n, 8192))
            if not chunk:
                continue
        now = time.time()
        dt = now - last_t
        if dt > max_gap[0]:
            max_gap = (dt, now - t0)
        last_t = now
        chunks.append("%.3f\t%d\t%s" % (now - t0, len(chunk), state))
        per_sec[int(now - t0)] += len(chunk)

        idx = 0
        while idx < len(chunk):
            if state == "WAIT_HEADER":
                b = chunk[idx]; idx += 1
                if b == 0x0A:
                    line = bytes(line_buf)
                    if line.startswith(b"$IMG,"):
                        try:
                            bin_expected = int(line[5:].strip())
                            state = "READING_BIN"; bin_buf = bytearray(); idx0 = idx
                            line_buf = bytearray()
                            continue
                        except ValueError:
                            pass
                    n_logline += 1
                    line_buf = bytearray()
                else:
                    if b < 0x09 or (0x0E <= b < 0x20) or b == 0x7F:
                        desync += 1
                    line_buf.append(b)
                    if len(line_buf) > 256 * 1024:
                        line_buf = bytearray()
            elif state == "READING_BIN":
                need = bin_expected - len(bin_buf)
                take = min(need, len(chunk) - idx)
                bin_buf.extend(chunk[idx:idx + take]); idx += take
                if len(bin_buf) >= bin_expected:
                    state = "READING_CRC"; crc_buf = bytearray()
            elif state == "READING_CRC":
                need = 4 - len(crc_buf)
                take = min(need, len(chunk) - idx)
                crc_buf.extend(chunk[idx:idx + take]); idx += take
                if len(crc_buf) == 4:
                    state = "WAIT_END"; end_buf = bytearray()
            else:
                b = chunk[idx]; idx += 1; end_buf.append(b)
                if b == 0x0A:
                    if bytes(end_buf).strip(b"\r\n") == b"$END":
                        n_frame += 1
                        sizes.append(len(bin_buf))
                        if len(crc_buf) == 4:
                            want = struct.unpack(">I", bytes(crc_buf))[0]
                            if (binascii.crc32(bytes(bin_buf)) & 0xFFFFFFFF) == want:
                                n_crc_ok += 1
                            else:
                                n_crc_bad += 1
                        state = "WAIT_HEADER"; line_buf = bytearray()
                    else:
                        if len(end_buf) > 128:
                            end_buf = bytearray()
    ser.close()
    total = sum(per_sec.values())
    os.makedirs(os.path.dirname(os.path.abspath(pref)) or ".", exist_ok=True)
    with open(pref + "_chunks.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(chunks))
    with open(pref + "_report.txt", "w", encoding="utf-8") as f:
        f.write("port=%s baud=%d dur=%.0fs\n" % (port, baud, dur))
        f.write("chunks=%d  total_bytes=%d  KB/s=%.2f\n" % (len(chunks), total, total / 1024.0 / dur))
        f.write("max_gap=%.3fs (at t=%.1fs)\n" % max_gap)
        f.write("frames=%d  crc_ok=%d  crc_bad=%d  jpg_avg=%d jpg_max=%d\n" % (
            n_frame, n_crc_ok, n_crc_bad, (sum(sizes) // max(1, len(sizes))), max(sizes or [0])))
        f.write("log_lines=%d  stray_ctrl_bytes_in_header_state=%d  end_state=%s\n" % (
            n_logline, desync, state))
        f.write("\nper-second bytes:\n")
        for s in sorted(per_sec):
            f.write("  %3ds  %6d B  %s\n" % (s, per_sec[s], "#" * min(60, per_sec[s] // 256)))
        f.write("\n最大的 10 个空档 (需要逐块算):\n")
        ts = [float(c.split("\t")[0]) for c in chunks]
        gaps = sorted(((ts[i] - ts[i - 1], ts[i]) for i in range(1, len(ts))), reverse=True)[:10]
        for g, t in gaps:
            f.write("  %.3fs @ t=%.1fs\n" % (g, t))
    print("report:", pref + "_report.txt")
    print("chunks=%d KB/s=%.2f maxgap=%.3fs frames=%d crc_ok=%d crc_bad=%d loglines=%d stray=%d end=%s" % (
        len(chunks), total / 1024.0 / dur, max_gap[0], n_frame, n_crc_ok, n_crc_bad, n_logline, desync, state))

main()

