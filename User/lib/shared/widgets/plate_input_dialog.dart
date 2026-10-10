import 'package:flutter/material.dart';

import '../../core/models/spot_model.dart';
import 'plate_thumb_view.dart';

/// ⭐ 车牌槽位 + 三阶段虚拟键盘输入弹窗 (替代系统 TextField).
///
/// 设计: 车牌是封闭集合, 不给开放文本框 —— 每字一格槽位显示, 键盘随当前格换肤:
///  ① 第 1 格 → 31 个省份汉字 (想输错都输不进)
///  ② 第 2 格 → 发牌机关字母 (去掉 I/O)
///  ③ 第 3 格起 → 序号: 数字行 + 字母行 (去掉 I/O)
///
/// 打开时输入区全空, 第 1 格默认选中, 从省份开始逐格输入.
///
/// 返回: 校验通过的车牌字符串; 用户取消/点遮罩返回 null.
/// [onRestore]: 顶部"恢复识别结果"入口 (仅存在人工修正时传入), 弹窗先自行关闭再回调.
Future<String?> showPlateInputDialog(
  BuildContext context, {
  required SpotModel spot,
  VoidCallback? onRestore,
}) {
  return showDialog<String>(
    context: context,
    barrierDismissible: true,
    builder: (ctx) => _PlateInputDialog(
      spot: spot,
      onRestore: onRestore,
    ),
  );
}

/* 与国标校验一致的省份集合 (31 个) */
const String kPlateProvinces = '京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼';

/* 发牌机关/序号字母: 24 个 (国标不含 I、O) */
const String kPlateLetters = 'ABCDEFGHJKLMNPQRSTUVWXYZ';

/* 国标车牌严格校验: 省份汉字 + 机关字母 + 序号 5 位
 * ⚠ 序号不允许全为字母 —— 至少含一个数字 (前瞻 (?=...[0-9]) 保证)
 * (输入上限 7 位, 8 位新能源不在此输入通道内, 故只保留普通车牌分支) */
final RegExp kPlatePattern = RegExp(
  r'^[京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼][A-Z](?=[A-Z0-9]*[0-9])[A-Z0-9]{5}$',
);

const Color _kPrimary = Color(0xFF0052D4);
const Color _kBorder = Color(0xFFE2E6EE);
const Color _kHint = Color(0xFF8A919F);

class _PlateInputDialog extends StatefulWidget {
  final SpotModel spot;
  final VoidCallback? onRestore;

  const _PlateInputDialog({
    required this.spot,
    this.onRestore,
  });

  @override
  State<_PlateInputDialog> createState() => _PlateInputDialogState();
}

class _PlateInputDialogState extends State<_PlateInputDialog> {
  static const int _maxLen = 7; // 普通车牌 7 位

  String _plate = ''; // 已输入的字符序列 (打开时全空)
  int _pos = 0; // 当前活动格 (0..maxLen), 默认第 1 格选中, 键盘阶段由它决定
  String? _error;

  /* ==================== 输入动作 ==================== */

  void _input(String ch) {
    if (_pos >= _maxLen) return;
    setState(() {
      _plate = _plate.substring(0, _pos) + ch + _plate.substring(_pos);
      _pos++;
      _error = null;
    });
  }

  void _backspace() {
    if (_pos == 0) return;
    setState(() {
      _plate = _plate.substring(0, _pos - 1) + _plate.substring(_pos);
      _pos--;
      _error = null;
    });
  }

  void _save() {
    final v = _plate;
    if (!kPlatePattern.hasMatch(v)) {
      setState(() => _error = v.length < 7
          ? '车牌未输完, 还差 ${7 - v.length} 位'
          : '格式不正确: 序号 5 位至少要含一个数字');
      return;
    }
    Navigator.pop(context, v);
  }

  /* ==================== 阶段提示 ==================== */

  String get _stageHint {
    if (_pos == 0) return '请选择省份';
    if (_pos == 1) return '请输入发牌机关代号';
    if (_pos >= _maxLen) return '输入完成, 请核对后保存';
    return '请输入序号';
  }

  /* ==================== 渲染 ==================== */

  @override
  Widget build(BuildContext context) {
    return Dialog(
      backgroundColor: Colors.transparent,
      insetPadding: const EdgeInsets.symmetric(horizontal: 24),
      child: Container(
        constraints: const BoxConstraints(maxWidth: 400),
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 12),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(16),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.18),
              blurRadius: 24,
              offset: const Offset(0, 8),
            ),
          ],
        ),
        child: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              _buildHeader(context),
              const SizedBox(height: 8),
              _buildEvidence(),
              const SizedBox(height: 10),
              _buildSlots(),
              if (_error != null) ...[
                const SizedBox(height: 4),
                Text(
                  _error!,
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 12, color: Color(0xFFEF4444)),
                ),
              ],
              const SizedBox(height: 6),
              Text(
                _stageHint,
                textAlign: TextAlign.center,
                style: const TextStyle(fontSize: 12, color: _kHint),
              ),
              const SizedBox(height: 6),
              _buildKeyboard(),
              const SizedBox(height: 10),
              _buildButtons(context),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildHeader(BuildContext context) {
    return Row(
      children: [
        const Icon(Icons.edit_outlined, size: 20, color: _kPrimary),
        const SizedBox(width: 8),
        const Text(
          '修正车牌号',
          style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
        ),
        const Spacer(),
        if (widget.onRestore != null)
          IconButton(
            onPressed: () {
              Navigator.pop(context);
              widget.onRestore!();
            },
            tooltip: '恢复节点识别结果',
            icon: const Icon(Icons.restart_alt, size: 20),
            color: _kHint,
          ),
      ],
    );
  }

  /// 证据区: 摄像头原图 (对照修改的依据), 无图不显示
  Widget _buildEvidence() {
    final spot = widget.spot;
    if (!spot.hasPlateThumb) return const SizedBox.shrink();
    return Column(
      children: [
        Center(
          child: FittedBox(
            fit: BoxFit.scaleDown,
            child: PlateThumbView(base64Data: spot.plateThumb!, pixelSize: 3),
          ),
        ),
        const SizedBox(height: 4),
        const Text(
          '摄像头原图 · 请对照修改',
          textAlign: TextAlign.center,
          style: TextStyle(fontSize: 11, color: _kHint),
        ),
      ],
    );
  }

  /// 槽位显示: 每字一格 — 纯展示, 不可手动点选;
  /// 光标只由输入推进 (输一个进一位, 退格退一位), 当前格蓝框提示
  Widget _buildSlots() {
    final boxes = <Widget>[];
    for (int i = 0; i < _maxLen; i++) {
      final filled = i < _plate.length;
      final active = i == _pos;
      boxes.add(
        Container(
          height: 40,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: active ? _kPrimary.withValues(alpha: 0.07) : Colors.white,
              borderRadius: BorderRadius.circular(8),
              border: Border.all(
                color: active
                    ? _kPrimary
                    : filled
                        ? const Color(0xFFB9C0CC)
                        : _kBorder,
                width: active ? 2 : 1.2,
              ),
            ),
            child: Text(
              filled ? _plate[i] : '',
              style: const TextStyle(
                fontSize: 19,
                fontWeight: FontWeight.w700,
                color: Color(0xFF1F2430),
                letterSpacing: 1,
              ),
            ),
        ),
      );
    }
    return Row(
      children: [
        for (int i = 0; i < boxes.length; i++) ...[
          if (i > 0) const SizedBox(width: 4),
          Expanded(child: boxes[i]),
        ],
      ],
    );
  }

  /* ==================== 三阶段虚拟键盘 ==================== */

  Widget _buildKeyboard() {
    final Widget keys;
    if (_pos == 0) {
      keys = _provinceKeys();
    } else if (_pos == 1) {
      keys = _letterKeys();
    } else {
      keys = _serialKeys();
    }
    return Column(
      children: [
        keys,
        const SizedBox(height: 6),
        /* 底部功能行: 退格 —— 全阶段可退格 */
        Row(
          children: [
            const Spacer(),
            _backspaceKey(),
          ],
        ),
      ],
    );
  }

  /// ① 省份汉字 (31 个)
  Widget _provinceKeys() {
    return Wrap(
      spacing: 4,
      runSpacing: 4,
      alignment: WrapAlignment.center,
      children: [
        for (final ch in kPlateProvinces.split('')) _charKey(ch, onTap: () => _input(ch)),
      ],
    );
  }

  /// ② 发牌机关字母 (24 个, 无 I/O)
  Widget _letterKeys() {
    return Wrap(
      spacing: 4,
      runSpacing: 4,
      alignment: WrapAlignment.center,
      children: [
        for (final ch in kPlateLetters.split('')) _charKey(ch, onTap: () => _input(ch)),
      ],
    );
  }

  /// ③ 序号: 数字行 + 字母行 (功能行在 _buildKeyboard 统一渲染)
  Widget _serialKeys() {
    return Column(
      children: [
        Row(
          children: [
            for (final d in '1234567890'.split(''))
              Expanded(child: _charKey(d, onTap: () => _input(d))),
          ],
        ),
        const SizedBox(height: 4),
        Wrap(
          spacing: 4,
          runSpacing: 4,
          alignment: WrapAlignment.center,
          children: [
            for (final ch in kPlateLetters.split('')) _charKey(ch, onTap: () => _input(ch)),
          ],
        ),
      ],
    );
  }

  Widget _charKey(String label, {required VoidCallback onTap}) {
    return GestureDetector(
      onTap: onTap,
      behavior: HitTestBehavior.opaque,
      child: Container(
        width: 28,
        height: 30,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(7),
          border: Border.all(color: _kBorder),
        ),
        child: Text(
          label,
          style: const TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.w500,
            color: Color(0xFF1F2430),
          ),
        ),
      ),
    );
  }

  Widget _backspaceKey() {
    return GestureDetector(
      onTap: _backspace,
      behavior: HitTestBehavior.opaque,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 7),
        decoration: BoxDecoration(
          color: const Color(0xFFF3F5F9),
          borderRadius: BorderRadius.circular(8),
        ),
        child: const Icon(Icons.backspace_outlined, size: 18, color: Color(0xFF5B6270)),
      ),
    );
  }

  Widget _buildButtons(BuildContext context) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.end,
      children: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('取消'),
        ),
        const SizedBox(width: 8),
        ElevatedButton(
          onPressed: _save,
          style: ElevatedButton.styleFrom(
            backgroundColor: _kPrimary,
            foregroundColor: Colors.white,
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
            ),
          ),
          child: const Text('保存'),
        ),
      ],
    );
  }
}
