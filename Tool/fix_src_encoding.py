#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix_src_encoding.py —— 把节点源码归一化为「UTF-8 带 BOM」

为什么需要它
------------
Keil 的 armcc V5 **只有看到 UTF-8 BOM** 才按 UTF-8 读源文件;
没有 BOM 就按系统 ANSI(中文 Windows = GBK/936) 读。于是
「UTF-8 无 BOM + 中文」会被误解析, 典型报错:

    app_tasks.c(214): error:  #8: missing closing quote
          case PARK_ZOMBIE:   return "鍍靛案杞?";

根因: 一个汉字在 UTF-8 里占 3 字节, 而 GBK 是 2 字节一配 —— 汉字个数为
**奇数**时最后会剩"半个字", 它去找结尾的引号配对, 把 \" 吃掉, 引号就没了。
个数为偶数时不报错, 但字符串内容已经乱码(更隐蔽的坑: 串口打印是乱的)。

而三个写入方各有各的默认编码:
    Keil 编辑器     -> GBK      (本机 C:\\Keil_v5\\UV4\\global.prop: code.page=936)
    VS Code / AI    -> UTF-8 无 BOM
混写必然炸。本脚本把源码统一到唯一安全态: **UTF-8 + BOM**。

(GBK 其实也是"编译器能读"的态, 但源码编码会一比一变成串口输出字节 ——
 GBK 源码 => 节点日志是 GBK 字节, 与摄像头(ESP32, UTF-8) 不一致,
 串口助手看两个设备要来回切编码。所以这里统一到 UTF-8+BOM。)

判定与动作
----------
    已有 UTF-8 BOM             -> 不动
    纯 ASCII                   -> 不动(任何编码读都一样, 不制造无谓 diff)
    UTF-8 无 BOM + 有非 ASCII  -> 补 BOM(只在文件头加 3 字节, 内容一字不动)
    能按 GBK 解、且非 UTF-8    -> 整体转码为 UTF-8 + BOM
    两种都判不出               -> 跳过 + 报错(退出码 2)

用法
----
    python Tool/fix_src_encoding.py --check    # 只报告, 不写任何文件
    python Tool/fix_src_encoding.py            # 执行归一化
    python Tool/fix_src_encoding.py <目录>...   # 覆盖默认扫描目录
    python Tool/fix_src_encoding.py --wire <工程.uvprojx>   # 给 Keil 工程接编译前钩子

默认扫描 = 仓库根下的 Node/ 与 shared/ + **自动发现的每个 Keil 工程根**
(工程文件所在目录的上一级: Node/Firmware/Project/App.uvprojx -> Node/Firmware)。
所以以后在仓库里任何地方新建 Keil 工程, 都会被自动纳入扫描, 不用改本文件;
新工程只要跑一次 --wire 把钩子接上即可 —— uVision 的 Before Make 是**工程级**
配置, 没有全局开关, 这是它唯一的限制。
不扫 Gateway/ 与 PlateRecognitionIDF/: 那两个是 GCC 工具链, 默认按 UTF-8 读,
加 BOM 是多余动作。
跳过目录: Output / Listing / Objects / build / obj / RTE / DebugConfig 等产物目录。

退出码: 0 = 全部处于安全态(或已修好); 2 = 有文件无法判定, 需人工看
"""
from __future__ import annotations

import os
import re
import sys

BOM = b"\xef\xbb\xbf"
DEFAULT_DIRS = ("Node", "shared")
SKIP_DIRS = {
    "Output", "Listing", "Objects", "build", "obj", "RTE",
    "DebugConfig", ".git", "__pycache__",
}
EXTS = (".c", ".h")


def find_root() -> str:
    """以本脚本位置为基准定位仓库根(Tool/ 的上一级)。"""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def iter_files(roots):
    for root in roots:
        for base, subdirs, names in os.walk(root):
            subdirs[:] = sorted(s for s in subdirs if s not in SKIP_DIRS)
            for name in sorted(names):
                if name.lower().endswith(EXTS):
                    yield os.path.join(base, name)


def classify(raw: bytes) -> str:
    if raw.startswith(BOM):
        return "utf8-bom"
    if not any(b > 0x7F for b in raw):
        return "ascii"
    try:
        raw.decode("utf-8")
        return "utf8-nobom"
    except UnicodeDecodeError:
        pass
    try:
        raw.decode("gbk")
        return "gbk"
    except UnicodeDecodeError:
        return "unknown"


def plan_of(kind: str):
    """返回 (动作名, 新字节 or None)。只改编码, 不动内容。"""
    if kind in ("utf8-bom", "ascii"):
        return "keep", None
    if kind == "utf8-nobom":
        return "补 BOM", "utf8-nobom"
    if kind == "gbk":
        return "GBK -> UTF-8+BOM", "gbk"
    return "无法判定", None


def new_bytes(raw: bytes, kind: str) -> bytes:
    """只改编码: 内容字节一字不动。"""
    if kind == "utf8-nobom":
        return BOM + raw
    return BOM + raw.decode("gbk").encode("utf-8")


def _is_inside(path: str, parent: str) -> bool:
    path, parent = os.path.abspath(path), os.path.abspath(parent)
    return path == parent or path.startswith(parent + os.sep)


def _rel_safe(path: str, base: str) -> str:
    """相对路径; 跨盘符时退回绝对路径(relpath 会抛 ValueError)。"""
    try:
        return os.path.relpath(path, base)
    except ValueError:
        return os.path.abspath(path)


def keil_project_roots(root: str, exclude=()):
    """自动发现仓库里每个 Keil 工程的源码根。

    工程文件一般在 <源码根>/Project/xxx.uvprojx, 所以取它所在目录的上一级。
    这样"新建一个 Keil 工程"不需要改本脚本 —— 扫描范围自己会跟上。
    """
    found = set()
    for base, subdirs, names in os.walk(root):
        subdirs[:] = sorted(s for s in subdirs if s not in SKIP_DIRS)
        if any(n.lower().endswith(".uvprojx") for n in names):
            found.add(os.path.dirname(base))
    out = []
    for d in sorted(found):
        if any(_is_inside(d, e) for e in list(exclude) + out):
            continue
        out.append(d)
    return out


BLOCK_RE = re.compile(r"<BeforeMake>.*?</BeforeMake>", re.S)


def wire_beforemake(text: str, cmd: str):
    """把 Before Make 钩子写进工程文件(文本级修改, 幂等)。返回 (新文本|None, 说明)。"""
    msgs = []

    def fix_one_block(m):
        block = m.group(0)
        if "fix_src_encoding.py" in block:
            # 已接好: 只顺手修掉那个会把构建搞死的 nStopB?X=1
            fixed = re.sub(r"(<nStopB[12]X>)1(</nStopB[12]X>)", r"\g<1>0\g<2>", block)
            msgs.append("钩子已接好, 无需改动" if fixed == block else "钩子已在; 顺带把 nStopB?X 由 1 修正为 0")
            return fixed
        for slot in ("1", "2"):
            if re.search(r"<UserProg%sName>\s*</UserProg%sName>" % (slot, slot), block):
                new = re.sub(r"<RunUserProg%s>0</RunUserProg%s>" % (slot, slot),
                             "<RunUserProg%s>1</RunUserProg%s>" % (slot, slot), block)
                # 用 lambda 做替换, 避免命令里的 Windows 反斜杠被当成正则转义
                new = re.sub(r"<UserProg%sName>\s*</UserProg%sName>" % (slot, slot),
                             lambda _m, s=slot: "<UserProg%sName>%s</UserProg%sName>" % (s, cmd, s),
                             new)
                new = re.sub(r"<nStopB%sX>1</nStopB%sX>" % (slot, slot),
                             "<nStopB%sX>0</nStopB%sX>" % (slot, slot), new)
                msgs.append("已接入 Before Make 槽 #%s" % slot)
                return new
        msgs.append("Before Make 两个槽都被占用, 未改动")
        return block

    new_text = BLOCK_RE.sub(fix_one_block, text)
    if not msgs:
        return None, "工程文件里没有 <BeforeMake> 段(工程文件格式可能不同), 未改动"
    return new_text, "; ".join(msgs)


def wire_project(root: str, proj_arg: str) -> int:
    """给指定 Keil 工程接上编译前钩子。"""
    proj = proj_arg if os.path.isabs(proj_arg) else os.path.join(os.getcwd(), proj_arg)
    if not os.path.isfile(proj):
        proj = os.path.join(root, proj_arg)
    if not os.path.isfile(proj):
        print("找不到工程文件: %s" % proj_arg)
        return 2

    script = os.path.abspath(__file__)
    rel = _rel_safe(script, os.path.dirname(proj))   # 跨盘符时退回绝对路径
    if " " in rel:                # 路径带空格必须加引号
        rel = '"%s"' % rel
    cmd = "python " + rel.replace("/", "\\")
    with open(proj, "rb") as f:
        raw = f.read()
    has_bom = raw.startswith(BOM)

    new_text, msg = wire_beforemake(raw.decode("utf-8-sig"), cmd)
    print("工程: %s" % _rel_safe(proj, root))
    print("命令: %s" % cmd)
    print("结果: %s" % msg)
    if new_text is None or new_text == raw.decode("utf-8-sig"):
        return 0
    out = new_text.encode("utf-8")
    if has_bom:
        out = BOM + out
    with open(proj, "wb") as f:
        f.write(out)
    print("已写回工程文件")
    return 0


def main() -> int:
    try:
        sys.stdout.reconfigure(errors="replace")  # 控制台是 GBK 也不炸
    except Exception:
        pass

    argv = sys.argv[1:]
    check = "--check" in argv
    root = find_root()

    # ---- 模式: 接线 (--wire <工程.uvprojx>) ----
    if "--wire" in argv:
        i = argv.index("--wire")
        if i + 1 >= len(argv):
            print("用法: python Tool/fix_src_encoding.py --wire <工程.uvprojx>")
            return 2
        return wire_project(root, argv[i + 1])

    given = [a for a in argv if not a.startswith("--")]
    if given:
        roots = [a if os.path.isabs(a) else os.path.join(root, a) for a in given]
    else:
        roots = [os.path.join(root, d) for d in DEFAULT_DIRS]
        proj_roots = keil_project_roots(root)
        extra = [d for d in proj_roots
                 if not any(_is_inside(d, r) for r in roots)]
        if proj_roots:
            print("检测到 Keil 工程根: %s%s"
                  % (", ".join(os.path.relpath(d, root) for d in proj_roots),
                     "" if extra else "  (已被上面的扫描目录覆盖)"))
        roots += extra

    for r in list(roots):
        if not os.path.isdir(r):
            print("[WARN] 目录不存在, 已跳过: %s" % r)
            roots.remove(r)
    if not roots:
        print("没有可扫描的目录")
        return 2

    counts, todo, bad = {}, [], []
    total = 0
    for path in iter_files(roots):
        total += 1
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError as exc:
            bad.append((path, "读不了: %s" % exc))
            continue
        kind = classify(raw)
        counts[kind] = counts.get(kind, 0) + 1
        act, _ = plan_of(kind)
        if kind == "unknown":
            bad.append((path, "既不是有效 UTF-8 也不是有效 GBK"))
        elif act != "keep":
            todo.append((path, act, kind))

    rel = lambda p: os.path.relpath(p, root)
    print("仓库根: %s" % root)
    print("扫描 : %s" % ", ".join(rel(r) for r in roots))
    print("共 %d 个 .c/.h" % total)
    for key, label in (("utf8-bom", "UTF-8+BOM (安全)"),
                       ("ascii", "纯 ASCII (安全)"),
                       ("utf8-nobom", "UTF-8 无 BOM (危险)"),
                       ("gbk", "GBK (需转换)"),
                       ("unknown", "无法判定")):
        if counts.get(key):
            print("   %-24s %d" % (label, counts[key]))
    print("")

    if not todo:
        print("无需处理: 所有文件都已在安全态")
    else:
        if check:
            print("待处理 %d 个文件 (--check 模式, 未改动任何文件):" % len(todo))
        else:
            print("已处理 %d 个文件:" % len(todo))
        for path, act, kind in todo:
            relpath = rel(path)
            if check:
                print("   [%s] %s" % (act, relpath))
                continue
            try:
                with open(path, "rb") as f:
                    raw = f.read()
                with open(path, "wb") as f:
                    f.write(new_bytes(raw, kind))
                print("   [%s] %s" % (act, relpath))
            except OSError as exc:
                bad.append((path, "写不了: %s" % exc))
                print("   [失败] %s (%s)" % (relpath, exc))

    if bad:
        print("")
        print("!! %d 个文件需要人工看一眼:" % len(bad))
        for path, why in bad:
            print("   [??] %s  (%s)" % (rel(path), why))
    if check:
        print("")
        print("(以上只是报告, 一个文件都没动)")
    return 2 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
