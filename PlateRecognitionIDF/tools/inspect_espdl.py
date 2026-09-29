# -*- coding: utf-8 -*-
"""离线体检 .espdl: 列出算子 / 输入输出 / 张量形状, 并比对运行端 esp-dl 白名单。

这是在烧板之前唯一能"看见模型内部"的手段 —— dl::Model 在设备上崩了
也只会给一句 Guru Meditation, 而这里能直接告诉你哪个节点运行端不认识。

用法:
    python inspect_espdl.py main/models/lprnet_s3.espdl
    python inspect_espdl.py x.espdl --ppq-site G:\\...\\venv\\Lib\\site-packages
"""

import argparse
import collections
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

DEFAULT_PPQ_SITE = r'G:\All_Project\AI_Project\LPRNet_Pytorch\venv\Lib\site-packages'


def decode(value):
    if value is None:
        return ''
    if isinstance(value, bytes):
        return value.decode('utf-8', 'replace')
    return str(value)


def load_flatbuffers(site_dir):
    """esp_ppq 自带 .espdl 的 FlatBuffers schema, 直接复用它来解析。"""
    schema_dir = os.path.join(site_dir, 'esp_ppq', 'parser', 'espdl')
    if not os.path.isdir(schema_dir):
        raise FileNotFoundError('找不到 esp_ppq espdl 解析器: %s' % schema_dir)
    if schema_dir not in sys.path:
        sys.path.insert(0, schema_dir)
    from FlatBuffers.Dl import Model as Mdl
    return Mdl


def split_edl2(path):
    """拆 EDL2 容器: "EDL2" + uint32 mode + uint32 size + uint32 padding + data"""
    with open(path, 'rb') as fp:
        blob = fp.read()
    magic = blob[:4]
    if magic != b'EDL2':
        raise ValueError('不是 EDL2 文件 (magic=%r); 加密模型无法离线解析' % magic)
    mode, size, _padding = struct.unpack('<III', blob[4:16])
    payload = blob[16:16 + size]
    if len(payload) != size:
        payload = blob[16:]
    return mode, payload


def attr_value(attr):
    """把 FlatBuffers Attribute 还原成 python 值 (尽力而为)。"""
    try:
        if attr.IntsLength():
            return list(attr.IntsAsNumpy())
    except Exception:
        pass
    try:
        if attr.FloatsLength():
            return [round(float(v), 6) for v in attr.FloatsAsNumpy()]
    except Exception:
        pass
    for getter in ('I', 'F'):
        try:
            value = getattr(attr, getter)()
        except Exception:
            value = None
        if value is not None:
            return value
    try:
        raw = attr.SAsNumpy()
        if raw is not None and len(raw):
            return bytes(raw).decode('utf-8', 'replace')
    except Exception:
        pass
    return '<type %s>' % attr.AttrType()


def shape_of(value_info):
    """ValueInfo -> [dims]; schema 层级较深, 只做尽力解析。"""
    try:
        type_info = value_info.ValueInfoType()
        if type_info is None:
            return None
        value = type_info.Value()
        if value is None:
            return None
        tensor_type = value.TensorType()
        if tensor_type is None:
            return None
        shape = tensor_type.Shape()
        if shape is None:
            return []
        return [shape.Dim(j).DimValue() for j in range(shape.DimLength())]
    except Exception:
        return None


def audit_initializers(graph, verbose=False):
    '''
    检查参数张量是否会让板端在取参数时崩溃。

    esp-dl 的 fbs::FbsModel::get_operation_parameter() 会直接读参数张量的 dims
    (反汇编定位到 fbs_model.cpp:649 的 l32i.n a9, a2, 0, 崩溃寄存器 A2=0),
    dims 为空的 0 维标量参数会在那里解引用空指针 -> LoadProhibited。

    所以 0 维参数 和 无数据参数 都是硬性故障, 必须在烧板前拦下。
    '''
    import helper

    if verbose:
        print('')
        print('initializer (参数张量, %d 个):' % graph.InitializerLength())
    empty_dims = []
    no_data = []
    for i in range(graph.InitializerLength()):
        t = graph.Initializer(i)
        if verbose:
            print('    ' + helper.printable_tensor(t).replace(chr(10), chr(10) + '    '))
        if t.DimsIsNone() or t.DimsLength() == 0:
            empty_dims.append(decode(t.Name()))
        has_data = not (t.RawDataIsNone() and t.FloatDataIsNone() and t.Int32DataIsNone()
                        and t.Int64DataIsNone() and t.Uint64DataIsNone() and t.DoubleDataIsNone())
        if not has_data:
            no_data.append(decode(t.Name()))

    print('')
    if empty_dims:
        print('!! %d 个 0 维 (dims 为空) 标量参数 ---- 板端取参数时空指针 (实测崩溃点)' % len(empty_dims))
        print('   %s' % ', '.join(empty_dims))
    else:
        print('参数形状检查: 无 0 维标量参数')
    if no_data:
        print('!! %d 个参数张量没有数据 ---- 同样会拿到空指针:' % len(no_data))
        print('   %s' % ', '.join(no_data))
    else:
        print('参数数据检查: 每个 initializer 都带数据')
    return empty_dims, no_data



def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('espdl', help='.espdl 文件路径')
    parser.add_argument('--ppq-site', default=DEFAULT_PPQ_SITE,
                        help='含 esp_ppq 的 site-packages 目录')
    parser.add_argument('--list-nodes', action='store_true', help='逐节点打印详情')
    parser.add_argument('--initializers', action='store_true',
                        help='列出所有 initializer (参数张量) 的形状/数据, 用于排查板端空指针')
    parser.add_argument('--graph', action='store_true',
                        help='用 esp_ppq 自带的 printable_graph 打印完整图 (含张量形状)')
    args = parser.parse_args()

    Mdl = load_flatbuffers(args.ppq_site)
    mode, payload = split_edl2(args.espdl)
    model = Mdl.Model.GetRootAs(payload, 0)
    graph = model.Graph()

    node_count = graph.NodeLength()
    ops = collections.Counter()

    shapes = {}
    for i in range(graph.ValueInfoLength()):
        info = graph.ValueInfo(i)
        shapes[decode(info.Name())] = shape_of(info)

    print('文件       : %s (%.1f KB)' % (args.espdl, os.path.getsize(args.espdl) / 1024))
    print('edl2 mode  : %d' % mode)
    print('节点总数   : %d' % node_count)
    print('initializer: %d' % graph.InitializerLength())
    print('graph 输入 : %d, 输出: %d' % (graph.InputLength(), graph.OutputLength()))

    for i in range(node_count):
        node = graph.Node(i)
        op_type = decode(node.OpType())
        ops[op_type] += 1
        if args.list_nodes:
            ins = [decode(node.Input(j)) for j in range(node.InputLength())]
            outs = [decode(node.Output(j)) for j in range(node.OutputLength())]
            print('')
            print('[%2d] %-18s %s' % (i, op_type, decode(node.Name())))
            print('     in : %s' % ', '.join('%s%s' % (n, shapes.get(n)) for n in ins))
            print('     out: %s' % ', '.join('%s%s' % (n, shapes.get(n)) for n in outs))
            for j in range(node.AttributeLength()):
                attr = node.Attribute(j)
                print('     attr %-18s = %s' % (decode(attr.Name()), attr_value(attr)))

    print('')
    print('算子分布:')
    for op_type, count in sorted(ops.items(), key=lambda kv: (-kv[1], kv[0])):
        print('    %-20s x %d' % (op_type, count))

    import helper

    empty_dims, no_data = audit_initializers(graph, verbose=args.initializers)

    print('graph 输入:')
    for i in range(graph.InputLength()):
        print('   ', helper.printable_value_info(graph.Input(i)))
    print('graph 输出:')
    for i in range(graph.OutputLength()):
        print('   ', helper.printable_value_info(graph.Output(i)))

    if args.graph:
        print('')
        print('-' * 72)
        print(helper.printable_graph(payload, print_value_info=True))

    import espdl_ops

    missing, registered = espdl_ops.check(ops, os.path.abspath(os.path.join(_HERE, '..')))
    print('')
    print('运行端 esp-dl 已注册算子 %d 个' % len(registered))
    failed = False
    if missing:
        print('!! 运行端未实现的算子 (会直接导致 dl::Model 构造失败):')
        for op_type, count in sorted(missing.items()):
            print('     %-20s x %d' % (op_type, count))
        failed = True
    if empty_dims or no_data:
        failed = True
    if failed:
        return 2
    print('体检通过: 算子 / 参数形状 / 参数数据 全部合规')
    return 0

if __name__ == '__main__':
    sys.exit(main())
