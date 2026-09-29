# -*- coding: utf-8 -*-
"""判定一个算子是否被运行端 esp-dl 支持 —— 唯一依据是运行端自己的注册表。

esp-dl 的算子白名单写在
    managed_components/espressif__esp-dl/dl/module/include/dl_module_creator.hpp
    -> ModuleCreator::register_dl_modules()

本模块直接从该头文件里把算子名抽出来, 避免人工抄写导致漏项。
"""

import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ESPDL_DIR = os.path.abspath(os.path.join(_HERE, '..'))

_REGISTER_RE = re.compile(r'register_module\(\s*"([^"]+)"')

# 这些算子只存在于 ONNX 前端, 导入阶段就会被 esp-ppq 折叠成 initializer /
# 形状常量, 不会以算子节点的形式进入设备端, 因此不需要运行端实现。
FRONTEND_ONLY_OPS = {
    'Constant',
    'ConstantOfShape',
    'Shape',
    'Identity',
}


def registered_ops(espdl_project_dir=DEFAULT_ESPDL_DIR):
    """解析 dl_module_creator.hpp, 返回运行端已实现的算子名集合。"""
    header = os.path.join(
        espdl_project_dir,
        'managed_components', 'espressif__esp-dl',
        'dl', 'module', 'include', 'dl_module_creator.hpp')
    if not os.path.isfile(header):
        raise FileNotFoundError('找不到运行端算子注册表: %s' % header)
    with open(header, 'r', encoding='utf-8', errors='replace') as fp:
        text = fp.read()
    return set(_REGISTER_RE.findall(text))


def check(ops, espdl_project_dir=DEFAULT_ESPDL_DIR):
    """返回 (缺失算子 -> 次数, 已支持算子集合)。"""
    ok = registered_ops(espdl_project_dir)
    missing = {}
    for op, count in ops.items():
        if op in FRONTEND_ONLY_OPS or op in ok:
            continue
        missing[op] = count
    return missing, ok


if __name__ == '__main__':
    ops = registered_ops()
    print('esp-dl 运行端已注册算子 (%d 个):' % len(ops))
    for name in sorted(ops):
        print('   ', name)
