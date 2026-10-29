#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
relocate_paths.py —— 把仓库里写死的绝对路径改成"跟着脚本走"的表达式

背景
----
项目从 G 盘搬到 D 盘之后，Keil 的 Before Make 钩子没坏（工程里本来就是
`python ..\..\..\Tool\fix_src_encoding.py` 这种相对写法，Keil 以 .uvprojx
所在目录为 cwd 执行，换盘/换目录都不受影响）。
真正坏掉的是 PlateRecognitionIDF/tools/ 和 analysis_out/ 里那批一次性实验脚本：
它们在模块顶层写死了 `r'G:\\All_Project\\AI_Project\\LPRNet_Pytorch'` 之类的
绝对路径，盘符一变就 FileNotFoundError。

本脚本把它们统一改写成从 __file__ 往上找仓库根的可移植表达式：

    r'G:\\All_Project\\AI_Project\\LPRNet_Pytorch'
        ->  _AI_ROOT_ + r'\\LPRNet_Pytorch'
    r'G:\\All_Project\\AI_Project\\Smart-Paring-Iot\\tools\\out\\x.onnx'
        ->  _REPO_ROOT_ + r'\\tools\\out\\x.onnx'

并注入一段自举前导（幂等，已经注入过的文件不会重复注入）：

    _REPO_ROOT_ / _AI_ROOT_ 由 __file__ 逐级上溯算出，与盘符、目录名无关。

用法
----
    python Tool/relocate_paths.py                 # 只体检，不改文件
    python Tool/relocate_paths.py --fix           # 改写 .py / .ps1
    python Tool/relocate_paths.py --fix --docs    # 连 .md / .txt 里的记录也改
    python Tool/relocate_paths.py --fix --dry-run # 打印将要做的事

退出码: 0 = 干净/已修好, 2 = 仍有残留(仅体检模式)
"""

import argparse
import ast
import io
import os
import re
import sys
import tokenize

# ---------------------------------------------------------------- 旧根 -> 新根
# 全部用"旧绝对前缀"描述；大小写不敏感，正反斜杠都认。
REPO_DIR_NAME = 'Smart-Paring-Iot'
OLD_REPO_PREFIXES = [
    r'G:\All_Project\AI_Project\Smart-Paring-Iot',
    r'g:\All_Project\AI_Project\Smart-Paring-Iot',
    r'G:\All_Project\Trae_Project\Smart-Paring-Iot',
    r'g:\All_Project\Trae_Project\Smart-Paring-Iot',
]
OLD_AI_PREFIXES = [
    r'G:\All_Project\AI_Project',
    r'g:\All_Project\AI_Project',
]

PREAMBLE_MARK = '_REPO_ROOT_ = _find_repo_root_(_HERE_)'

PREAMBLE = '''
# --- 可移植路径自举 (Tool/relocate_paths.py 注入, 勿手改) -------------------
import os as _os

_HERE_ = _os.path.dirname(_os.path.abspath(__file__))


def _find_repo_root_(start):
    """从 start 逐级上溯, 找到同时含 Tool/ 与 Node/ 的目录 = 仓库根。"""
    cur = start
    while True:
        if _os.path.isdir(_os.path.join(cur, 'Tool')) and \\
           _os.path.isdir(_os.path.join(cur, 'Node')):
            return cur
        parent = _os.path.dirname(cur)
        if parent == cur:
            return _os.path.dirname(start)
        cur = parent


_REPO_ROOT_ = _find_repo_root_(_HERE_)
_AI_ROOT_ = _os.path.dirname(_REPO_ROOT_)
# --- 自举结束 ---------------------------------------------------------------
'''.strip('\n')


def norm(text):
    """统一成反斜杠、小写，用于前缀匹配。"""
    return text.replace('/', '\\').lower()


def match_prefix(value):
    """value(已是反斜杠形式) 命中哪个旧前缀? 返回 (kind, prefix_len) 或 None。"""
    low = norm(value)
    for p in OLD_REPO_PREFIXES:
        if low.startswith(norm(p)):
            return 'repo', len(p)
    for p in OLD_AI_PREFIXES:
        if low.startswith(norm(p)):
            return 'ai', len(p)
    return None


def build_expr(value):
    """把旧绝对路径字符串值转成可移植表达式; 不认识则返回 None。"""
    hit = match_prefix(value)
    if not hit:
        return None
    kind, plen = hit
    tail = value[plen:]
    tail = tail.replace('/', '\\').lstrip('\\')
    base = '_REPO_ROOT_' if kind == 'repo' else '_AI_ROOT_'
    if not tail:
        return base
    return '%s + r\'\\%s\'' % (base, tail)


def _docstring_starts(tree):
    """收集所有 docstring 的 (行, 列) 起点，用来跳过它们。

    不能用"行首的字符串就是 docstring"来猜：`roots = [\\n  r'...'` 这种
    列表元素也落在行首。所以直接问 AST 哪些 Expr 是 docstring。
    """
    out = set()
    holders = (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
    for node in ast.walk(tree):
        if not isinstance(node, holders) or not node.body:
            continue
        first = node.body[0]
        if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) \
                and isinstance(first.value.value, str):
            out.add((first.value.lineno, first.value.col_offset))
    return out


def _is_docstring_positions(tree):
    """返回模块级 docstring 结束行号(0 表示没有)。"""
    if tree.body and isinstance(tree.body[0], ast.Expr):
        node = tree.body[0].value
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return getattr(node, 'end_lineno', 0) or 0
    return 0


def _future_import_end(tree):
    end = 0
    for node in tree.body:
        if isinstance(node, ast.ImportFrom) and node.module == '__future__':
            end = max(end, getattr(node, 'end_lineno', 0) or 0)
        elif isinstance(node, (ast.Expr, ast.Import, ast.ImportFrom)):
            continue
        else:
            break
    return end


def _already_injected(src):
    return PREAMBLE_MARK in src


def _coding_cookie_row(src):
    """`# -*- coding: utf-8 -*-` 必须留在前两行内，否则失效。返回该行号(0=没有)。"""
    for i, line in enumerate(src.splitlines()[:2], start=1):
        if re.match(r'^[ \t\f]*#.*?coding[:=][ \t]*([-_.a-zA-Z0-9]+)', line):
            return i
    return 0


def _shebang_row(src):
    first = src.split('\n', 1)[0]
    return 1 if first.startswith('#!') else 0


def _insert_row(tree, src):
    """前导应插在哪一行之后：shebang / 编码声明 / docstring / __future__ 之后。"""
    return max(_is_docstring_positions(tree),
               _future_import_end(tree),
               _coding_cookie_row(src),
               _shebang_row(src))


def rewrite_py(path, apply_changes, dry_run):
    with open(path, 'rb') as f:
        raw = f.read()
    # 保留原 BOM / 编码
    bom = b''
    if raw.startswith(b'\xef\xbb\xbf'):
        bom, raw = raw[:3], raw[3:]
    try:
        src = raw.decode('utf-8')
    except UnicodeDecodeError:
        return ('skip-encoding', 0, 0)

    try:
        tree = ast.parse(src)
    except SyntaxError as e:
        return ('skip-syntax:%s' % e.msg, 0, 0)

    lines = src.splitlines(keepends=True)
    # 行首偏移
    offsets = [0]
    for ln in lines:
        offsets.append(offsets[-1] + len(ln))

    edits = []          # (start_abs, end_abs, replacement)
    doc_starts = _docstring_starts(tree)
    in_fstring = 0
    at_stmt_start = True
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(src).readline))
    except (tokenize.TokenError, IndentationError):
        return ('skip-tokenize', 0, 0)

    for tok in toks:
        ttype, tstr = tok.type, tok.string
        if ttype == getattr(tokenize, 'FSTRING_START', -1):
            in_fstring += 1
            at_stmt_start = False
            continue
        if ttype == getattr(tokenize, 'FSTRING_END', -1):
            in_fstring = max(0, in_fstring - 1)
            at_stmt_start = False
            continue
        if ttype == tokenize.STRING and not in_fstring:
            is_doc = (tok.start in doc_starts)
            at_stmt_start = False
            if is_doc:
                continue                     # 模块/函数 docstring 不动
            try:
                val = ast.literal_eval(tstr)
            except (ValueError, SyntaxError):
                continue
            if not isinstance(val, str):
                continue
            expr = build_expr(val)
            if expr is None:
                continue
            s = offsets[tok.start[0] - 1] + tok.start[1]
            e = offsets[tok.end[0] - 1] + tok.end[1]
            edits.append((s, e, expr))
            continue
        if ttype in (tokenize.NEWLINE, tokenize.NL, tokenize.INDENT,
                     tokenize.DEDENT, tokenize.ENCODING, tokenize.COMMENT):
            if ttype in (tokenize.NEWLINE, tokenize.NL, tokenize.INDENT,
                         tokenize.DEDENT):
                at_stmt_start = True
            continue
        at_stmt_start = False

    need_preamble = bool(edits) and not _already_injected(src)

    if not edits and not need_preamble:
        return ('clean', 0, 0)
    if dry_run:
        return ('would-fix', len(edits), 1 if need_preamble else 0)
    if not apply_changes:
        return ('needs-fix', len(edits), 1 if need_preamble else 0)

    out = src
    for s, e, rep in sorted(edits, key=lambda x: x[0], reverse=True):
        out = out[:s] + rep + out[e:]

    if need_preamble:
        lines = out.splitlines(keepends=True)
        row = _insert_row(tree, out)         # 在该行之后插入
        if row <= 0:
            row = 0
        head = ''.join(lines[:row])
        if head and not head.endswith('\n'):
            head += '\n'
        body = ''.join(lines[row:])
        if body.startswith('\n'):
            body = body[1:]
        out = head + PREAMBLE + '\n\n' + body

    with open(path, 'wb') as f:
        f.write(bom + out.encode('utf-8'))
    return ('fixed', len(edits), 1 if need_preamble else 0)


def rewrite_ps1(path, apply_changes, dry_run):
    with open(path, 'rb') as f:
        raw = f.read()
    bom = b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b''
    src = (raw[3:] if bom else raw).decode('utf-8', 'replace')
    hits = []
    for p in OLD_REPO_PREFIXES:
        for sep in ('\\', '/'):
            variant = p.replace('\\', sep)
            if variant in src:
                hits.append(variant)
    if not hits:
        return ('clean', 0, 0)
    if dry_run:
        return ('would-fix', len(hits), 1)
    if not apply_changes:
        return ('needs-fix', len(hits), 1)
    out = src
    for v in hits:
        out = out.replace(v, "$(Split-Path -Parent (Split-Path -Parent $PSScriptRoot))")
    with open(path, 'wb') as f:
        f.write(bom + out.encode('utf-8'))
    return ('fixed', len(hits), 1)


def rewrite_doc(path, apply_changes, dry_run):
    with open(path, 'rb') as f:
        raw = f.read()
    bom = b'\xef\xbb\xbf' if raw.startswith(b'\xef\xbb\xbf') else b''
    src = (raw[3:] if bom else raw).decode('utf-8', 'replace')
    n = 0
    for p in OLD_REPO_PREFIXES + OLD_AI_PREFIXES:
        for sep in ('\\', '/'):
            variant = p.replace('\\', sep)
            n += src.count(variant)
    if not n:
        return ('clean', 0, 0)
    if dry_run:
        return ('would-fix', n, 0)
    if not apply_changes:
        return ('needs-fix', n, 0)
    out = src
    for p in OLD_REPO_PREFIXES:
        for sep in ('\\', '/'):
            out = out.replace(p.replace('\\', sep), '<仓库根>')
    for p in OLD_AI_PREFIXES:
        for sep in ('\\', '/'):
            out = out.replace(p.replace('\\', sep), '<AI_Project>')
    with open(path, 'wb') as f:
        f.write(bom + out.encode('utf-8'))
    return ('fixed', n, 0)


SKIP_DIRS = {'build', '.git', 'venv', '__pycache__', 'managed_components',
             '.cache', 'out', 'Output', 'Listing', 'Objects', 'DebugConfig',
             'RTE', 'node_modules', '.dsh'}


def walk(root, exts):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith('.git')]
        for name in filenames:
            if name.lower().endswith(exts):
                yield os.path.join(dirpath, name)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='把仓库里写死的绝对路径改成跟着 __file__ 走的可移植写法')
    ap.add_argument('--fix', action='store_true', help='真的改文件')
    ap.add_argument('--docs', action='store_true', help='连 .md/.txt 一起改')
    ap.add_argument('--dry-run', action='store_true', help='只打印计划')
    ap.add_argument('--root', default=None, help='仓库根(默认按脚本位置推断)')
    args = ap.parse_args(argv)

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    self_path = os.path.abspath(__file__)

    groups = [('.py', rewrite_py), ('.ps1', rewrite_ps1)]
    if args.docs:
        groups += [('.md', rewrite_doc), ('.txt', rewrite_doc)]

    buckets = {}
    total_fix = 0
    for ext, fn in groups:
        for path in walk(root, (ext,)):
            if os.path.abspath(path) == self_path:
                continue
            status, n_str, n_pre = fn(path, args.fix, args.dry_run)
            key = status.split(':')[0]
            buckets.setdefault(key, []).append(
                (os.path.relpath(path, root), n_str, n_pre))
            if key in ('fixed', 'would-fix'):
                total_fix += 1

    print('仓库根: %s' % root)
    print('AI 根  : %s' % os.path.dirname(root))
    print('')
    order = ['fixed', 'would-fix', 'needs-fix', 'clean', 'skip-encoding',
             'skip-syntax', 'skip-tokenize']
    for key in order:
        items = buckets.get(key, [])
        if not items:
            continue
        print('[%s] %d 个文件' % (key, len(items)))
        if key in ('fixed', 'would-fix', 'needs-fix'):
            for rel, n_str, n_pre in sorted(items):
                print('    %-62s 字面量 %-3d 前导 %s' % (rel, n_str, n_pre))
        elif key.startswith('skip'):
            for rel, _, _ in sorted(items)[:20]:
                print('    %s' % rel)
        print('')

    remaining = [b for k in ('needs-fix', 'would-fix') for b in buckets.get(k, [])]
    if args.fix:
        print('已改写 %d 个文件。' % len(buckets.get('fixed', [])))
        return 0
    if remaining:
        print('仍有 %d 个文件含写死的绝对路径；加 --fix 处理。' % len(remaining))
        return 2
    print('全部干净：没有写死的绝对路径。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
