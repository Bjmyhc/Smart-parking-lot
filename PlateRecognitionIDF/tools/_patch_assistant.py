# -*- coding: utf-8 -*-
"""_patch_assistant.py -- BY串口助手: 收帧看门狗 + 图片队列丢最旧并记账 + CRC 失败可见。"""
import io, sys
P = r"G:\All_Project\AI_Project\BY串口助手\serial_img_tool.py"
src = io.open(P, encoding="utf-8", newline="").read()
n0 = len(src)
OPS = []

# ---- A1: 在 _read_loop 里加 "最后收到字节的时刻" ----
OPS.append((
"""        state = "WAIT_HEADER"
        bin_expected = 0
""",
"""        state = "WAIT_HEADER"
        bin_expected = 0
        last_byte_t = time.time()      # A1: 收帧看门狗用 —— 最后一次收到字节的时刻
        frame_wd_resets = 0            # A1: 本次会话因超时复位了几次
"""))

# ---- A1: 看门狗本体 (每次拿到数据块之前先判) ----
OPS.append((
"""            # ---- 取证: 会话首包缓冲, 攒够 512 字节或 0.3s 后把首批原始字节交给主线程 ----
""",
"""            # ---- A1: 收帧看门狗 ----
            #   以前一旦流里少一个字节(USB 抖动 / 传输中途关端口 / 驱动丢包), 读线程会**永远**
            #   停在 READING_BIN 等那 len 个字节 —— 图片链路永久失联, 只有重开端口才能恢复,
            #   界面上表现就是"图片卡着不动", 而日志照常滚。这里加一个兜底:
            #   只要"卡在收帧中间"且 3 秒内一个字节都没来, 就把解析器复位到 WAIT_HEADER 重新找帧头。
            now_t = time.time()
            if state != "WAIT_HEADER" and (now_t - last_byte_t) > 3.0:
                frame_wd_resets += 1
                try:
                    self._qput_raw(("\\r\\n[助手] 收帧超时: 停在 %s %.1fs 无新字节 (已收 %d/%d), "
                                    "自动复位解析器(第 %d 次)\\r\\n"
                                    % (state, now_t - last_byte_t, len(bin_buf), bin_expected,
                                       frame_wd_resets)).encode(self._encoding, "replace"))
                except Exception:
                    pass
                state = "WAIT_HEADER"
                bin_buf = bytearray(); crc_buf = bytearray(); end_buf = bytearray()
                line_buf = bytearray(); bin_expected = 0
            last_byte_t = now_t

            # ---- 取证: 会话首包缓冲, 攒够 512 字节或 0.3s 后把首批原始字节交给主线程 ----
"""))

# ---- A2: 图片队列满 -> 丢最旧, 并记账 ----
OPS.append((
"""                            try:
                                # 帧处理(含图片解码/预览/OCR等)统一交给主线程,
                                # 读取线程不直接调用任何 Tk 操作
                                self.bin_frame_queue.put(
                                    (bytes(bin_buf), bytes(crc_buf)), timeout=1.0)
                            except queue.Full:
                                pass
""",
"""                            # A2: 队列满以前是 `except queue.Full: pass` —— **静默丢帧**,
                            #   界面上只看到"图片不动了"却查不到原因。现在: 丢**最旧**的一帧,
                            #   把最新这帧放进去(预览要的是"现在"), 并且记一笔账给主线程报。
                            _frame = (bytes(bin_buf), bytes(crc_buf))
                            try:
                                self.bin_frame_queue.put_nowait(_frame)
                            except queue.Full:
                                try:
                                    self.bin_frame_queue.get_nowait()
                                except queue.Empty:
                                    pass
                                try:
                                    self.bin_frame_queue.put_nowait(_frame)
                                except queue.Full:
                                    pass
                                self._img_drop_n = getattr(self, "_img_drop_n", 0) + 1
"""))

# ---- A2b: 主线程里把丢帧数报出来 (限流 3s 一次) ----
OPS.append((
"""            self._handle_binary_frame(jpg_data, crc_bytes)
            n += 1
""",
"""            self._handle_binary_frame(jpg_data, crc_bytes)
            n += 1
        _drop = getattr(self, "_img_drop_n", 0)
        if _drop != getattr(self, "_img_drop_reported", 0) and \\
                (time.time() - getattr(self, "_img_drop_t", 0.0)) > 3.0:
            self._img_drop_reported = _drop
            self._img_drop_t = time.time()
            self._diag_log("[诊断] 图片队列丢帧累计 %d 帧 (主线程来不及处理, 只保留最新帧)" % _drop)
"""))

# ---- A3: CRC 失败无条件计数 (原来预览关掉时完全静默) ----
OPS.append((
"""        if actual_crc != expected_crc:
            if self.comp_preview.get():
                self._post_image_log(f"CRC校验失败, 期望=0x{expected_crc:08X} 实际=0x{actual_crc:08X}, 丢弃 {len(jpg_data)} 字节")
            return
""",
"""        if actual_crc != expected_crc:
            if self.comp_preview.get():
                self._post_image_log(f"CRC校验失败, 期望=0x{expected_crc:08X} 实际=0x{actual_crc:08X}, 丢弃 {len(jpg_data)} 字节")
            # A3: 校验失败以前在"预览"未打开时是**完全静默**的 —— 现在无条件计数 + 限流上报
            self._crc_bad_n = getattr(self, "_crc_bad_n", 0) + 1
            if (time.time() - getattr(self, "_crc_bad_t", 0.0)) > 3.0:
                self._crc_bad_t = time.time()
                self._diag_log("[诊断] 本会话 JPEG 帧 CRC 校验失败累计 %d 次 (坏帧已丢弃)"
                               % self._crc_bad_n)
            return
"""))

bad = 0
for i, (old, new) in enumerate(OPS):
    c = src.count(old)
    if c != 1:
        print("ANCHOR %d count=%d -> ABORT" % (i, c)); bad += 1
if bad:
    sys.exit(1)
for old, new in OPS:
    src = src.replace(old, new, 1)
io.open(P, "w", encoding="utf-8", newline="").write(src)
print("OK patches=%d  %d -> %d bytes" % (len(OPS), n0, len(src)))
