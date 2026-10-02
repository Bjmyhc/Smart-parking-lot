#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
node_sim.py —— 在电脑上扮演「节点单片机」(固件 P5.68 配套)

现实约束: 手上还没有节点板, 摄像头那条链路暂时复用控制台串口。
这个脚本就是那个「假节点」: 打开串口 -> 发 AT 命令 -> 把收到的 $PLATE 行挑出来打印。

用法:
    python node_sim.py --list                      # 先看有哪些串口
    python node_sim.py COM10                       # 连上后交互: 直接敲 AT 命令回车即发
    python node_sim.py COM10 --test "豫F·SQ818"    # 每 2 秒注入一次测试车牌 (验证整条上行链路)
    python node_sim.py COM10 --send "AT+INFO"      # 只发一条, 收 3 秒就退出

注意:
  - 波特率默认 921600, 和固件控制台一致; 用 --baud 改。
  - 固件的 AT 命令最快也要约 0.9s 才回 (主循环一轮识别的时间), 别急着判定超时。
  - 串口里混着固件日志和 $IMG 图片帧; 本脚本自动跳过图片的二进制负载, 只显示有用的行。
  - 控制权 (P5.69): 上电默认「正常模式」= 节点是主人, 电脑口只能发 AT+CTRL。
    电脑要发命令先 `AT+CTRL=PC` 切到调试模式; 切回 `AT+CTRL=NODE` 会强制关掉串口图片。
"""
import argparse
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺 pyserial:  pip install pyserial")


class FrameFilter:
    """把串口字节流切成「行」, 顺便整段跳过 $IMG 帧的二进制负载 (JPEG + CRC32)"""

    def __init__(self):
        self.buf = bytearray()
        self.skip = 0          # 还要原样丢掉的字节数

    def feed(self, data):
        self.buf += data
        lines = []
        while True:
            if self.skip > 0:
                take = min(self.skip, len(self.buf))
                del self.buf[:take]
                self.skip -= take
                if self.skip > 0:
                    break
                continue
            i = self.buf.find(b"\n")
            if i < 0:
                if len(self.buf) > 8192:      # 防御: 一直没有换行的垃圾流
                    del self.buf[:-256]
                break
            line = bytes(self.buf[:i]).rstrip(b"\r")
            del self.buf[:i + 1]
            s = line.decode("utf-8", "replace").strip()
            if s.startswith("$IMG,"):
                try:
                    self.skip = int(s[5:]) + 4        # JPEG 正文 + 4 字节 CRC32
                except ValueError:
                    self.skip = 0
                continue
            if s == "$END" or not s:
                continue
            lines.append(s)
        return lines


def interesting(s):
    """只看和链路有关的行, 把固件日志刷屏挡掉"""
    if s in ("OK",):
        return True
    if s.startswith("ERR"):                     # P5.74: 错误回复统一改成 ERR:<原因>
        return True
    if s.startswith("+"):                       # +INFO / +PLATE / +LOG / +IMG / +PAD / +HELP
        return True
    if s.startswith("$PLATE"):
        return True
    return False


def reader_loop(ser, stop):
    flt = FrameFilter()
    while not stop.is_set():
        try:
            chunk = ser.read(ser.in_waiting or 1)
        except Exception as exc:                # 拔线/端口被关
            print("\n[串口断开] %s" % exc)
            stop.set()
            return
        if not chunk:
            continue
        for s in flt.feed(chunk):
            if not interesting(s):
                continue
            if s.startswith("$PLATE"):
                print("\n  >>> %s" % s)         # 上行数据: 高亮
            else:
                print("      %s" % s)


def test_loop(ser, plate, period, stop):
    while not stop.is_set():
        if stop.wait(period):
            return
        try:
            ser.write(("AT+TEST=%s\n" % plate).encode("utf-8"))
        except Exception:
            return


def main():
    ap = argparse.ArgumentParser(description="在电脑上扮演节点单片机, 收发摄像头链路")
    ap.add_argument("port", nargs="?", help="串口号, 例如 COM10")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--list", action="store_true", help="列出串口后退出")
    ap.add_argument("--send", metavar="CMD", help="只发这一条命令, 收 --wait 秒后退出")
    ap.add_argument("--test", metavar="PLATE", help="每 --period 秒注入一次这个测试车牌")
    ap.add_argument("--period", type=float, default=2.0)
    ap.add_argument("--wait", type=float, default=3.0)
    args = ap.parse_args()

    if args.list:
        ports = list(list_ports.comports())
        if not ports:
            print("没找到串口 (设备插了吗?)")
        for p in ports:
            print("%-8s %s" % (p.device, p.description))
        return 0

    if not args.port:
        ap.error("要么给串口号, 要么用 --list")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except Exception as exc:
        print("打不开 %s @%d: %s" % (args.port, args.baud, exc))
        return 1

    print("已连接 %s @%d。Ctrl-C 退出。" % (args.port, args.baud))
    print("提示: 上电默认是「正常模式」, 电脑口只读 —— 先发 AT+CTRL=PC 解锁, 才能发别的命令")
    print("      AT / AT+HELP / AT+INFO / AT+PLATE / AT+TEST=<车牌> / AT+RUN / AT+TRIG(=0 连续) / AT+CTRL=NODE|PC")
    print("      任意命令后面加 ? 看它的取值含义 (例如 AT+IMG?); 设置回 OK, 查询只回 +XXX:<当前值>")
    stop = threading.Event()
    threading.Thread(target=reader_loop, args=(ser, stop), daemon=True).start()

    if args.test:
        threading.Thread(target=sim_loop, args=(ser, args.test, args.period, stop), daemon=True).start()

    try:
        if args.send:
            ser.write((args.send + "\n").encode("utf-8"))
            time.sleep(args.wait)
            return 0
        while not stop.is_set():
            cmd = input()
            if not cmd.strip():
                continue
            ser.write((cmd.strip() + "\n").encode("utf-8"))
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        try:
            ser.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())