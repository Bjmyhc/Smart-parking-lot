import 'package:flutter/material.dart';
import 'package:flutter/cupertino.dart';
import 'package:provider/provider.dart';
import '../../../../core/models/policy_config.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../shared/widgets/card_container.dart';

/// 策略配置页 (极简卡片风格): 告警/传感器/刷新/OTA 四组策略, 修改即时生效并持久化.
class PolicyConfigPage extends StatefulWidget {
  const PolicyConfigPage({super.key});

  @override
  State<PolicyConfigPage> createState() => _PolicyConfigPageState();
}

class _PolicyConfigPageState extends State<PolicyConfigPage> {
  static const _cooldownMs = 5000;  /* 下发冷却 5 秒 */
  bool _isDispatching = false;       /* 是否正在下发(控制 loading) */
  int _lastDispatchAt = 0;           /* 上次下发时间戳(millisecondsSinceEpoch) */

  /// 检查冷却, 返回剩余秒数 (0=可下发)
  int _cooldownLeft() {
    final elapsed = DateTime.now().millisecondsSinceEpoch - _lastDispatchAt;
    final remaining = (_cooldownMs - elapsed) ~/ 1000;
    return remaining > 0 ? remaining : 0;
  }

  /// 执行节点策略下发, 带 loading + 冷却
  Future<void> _dispatchNodePolicy(
    Future<bool> Function() action, {
    required VoidCallback onSuccess,
    required VoidCallback onFailure,
  }) async {
    if (_isDispatching) return;
    /* ⭐ 先查缓存的网关在线状态, 离线直接拦截 */
    if (!context.read<ParkingProvider>().gatewayOnline) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('网关离线，无法下发节点策略'), backgroundColor: Colors.red, duration: Duration(seconds: 2)),
      );
      return;
    }
    final cd = _cooldownLeft();
    if (cd > 0) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('操作太频繁，请 ${cd} 秒后再试'), backgroundColor: Colors.orange, duration: Duration(seconds: 2)),
      );
      return;
    }
    setState(() => _isDispatching = true);
    final success = await action();
    if (!mounted) return;
    setState(() {
      _isDispatching = false;
      _lastDispatchAt = DateTime.now().millisecondsSinceEpoch;
    });
    if (success) onSuccess();
    else onFailure();
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final policy = provider.policy;

    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(
        backgroundColor: Colors.transparent,
        elevation: 0,
        centerTitle: true,
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: AppColors.textPrimary),
          onPressed: () => Navigator.pop(context),
        ),
        title: const Text(
          '策略配置',
          style: TextStyle(
            color: AppColors.textPrimary,
            fontSize: 17,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
      body: Stack(
        children: [
          SafeArea(
            child: SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Padding(
                    padding: const EdgeInsets.only(left: 4, bottom: 12),
                    child: Text(
                      '修改后即时生效，无需重启',
                      style: TextStyle(
                        fontSize: 12,
                        color: AppColors.textSecondary.withValues(alpha: 0.8),
                      ),
                    ),
                  ),
                  _buildNodeStrategyGroup(context, provider, policy),
                  const SizedBox(height: 12),
                  _buildPlatformStrategyGroup(context, provider, policy),
                  const SizedBox(height: 20),
                  _buildResetButton(context, provider),
                ],
              ),
            ),
          ),
          if (_isDispatching)
            Positioned.fill(
              child: Container(
                color: Colors.black.withValues(alpha: 0.3),
                child: const Center(
                  child: Card(
                    child: Padding(
                      padding: EdgeInsets.all(20),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          SizedBox(
                            width: 32,
                            height: 32,
                            child: CircularProgressIndicator(strokeWidth: 3),
                          ),
                          SizedBox(height: 12),
                          Text('正在下发...'),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  /* ==================== 组卡片 ==================== */

  /// 节点策略: 需要网关在线才能下发到节点设备
  Widget _buildNodeStrategyGroup(BuildContext context, ParkingProvider provider, PolicyConfig policy) {
    final gatewayOnline = provider.gatewayOnline;
    return _buildGroup(
      icon: Icons.memory_outlined,
      title: '节点策略',
      description: gatewayOnline ? '下发到节点设备 (STM32) 立即生效' : '网关离线，无法修改',
      rows: [
        _buildDurationRow(
          context: context,
          title: '僵尸车判定阈值',
          subtitle: '节点端占用超过该时长判定为僵尸车',
          totalSeconds: policy.zombieThresholdSec,
          enabled: gatewayOnline,
          onChanged: (totalSeconds) {
            _dispatchNodePolicy(
              () => provider.updatePolicy(policy.copyWith(zombieThresholdSec: totalSeconds)),
              onSuccess: () {
                final Color bgColor;
                final String message;
                if (provider.policyPartial) {
                  bgColor = Colors.amber.shade700;
                  message = '⚠️ 部分下发：${provider.policyError ?? '成功一部分节点'}';
                } else {
                  bgColor = Colors.green;
                  message = '✅ 僵尸车判定阈值已下发至节点';
                }
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text(message), backgroundColor: bgColor, duration: const Duration(seconds: 2)),
                );
              },
              onFailure: () {
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text('❌ 修改失败：${provider.policyError ?? '未知错误'}'), backgroundColor: Colors.red, duration: const Duration(seconds: 2)),
                );
              },
            );
          },
        ),
        _buildInputRow(
          context: context,
          title: '超声波判定距离阈值',
          subtitle: '距离 < 阈值判为有车，范围 2-400cm',
          value: policy.sensorDistanceCm,
          unit: 'cm',
          min: 2,
          max: 400,
          enabled: gatewayOnline,
          onSubmitted: (v) {
            if (v == policy.sensorDistanceCm) return;  /* 值没变, 跳过 */
            _dispatchNodePolicy(
              () => provider.updatePolicy(policy.copyWith(sensorDistanceCm: v)),
              onSuccess: () {
                final Color bgColor;
                final String message;
                if (provider.policyPartial) {
                  bgColor = Colors.amber.shade700;
                  message = '⚠️ 部分下发：${provider.policyError ?? '成功一部分节点'}';
                } else {
                  bgColor = Colors.green;
                  message = '✅ 超声波距离阈值已下发至节点';
                }
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text(message), backgroundColor: bgColor, duration: const Duration(seconds: 2)),
                );
              },
              onFailure: () {
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(content: Text('❌ 修改失败：${provider.policyError ?? '未知错误'}'), backgroundColor: Colors.red, duration: const Duration(seconds: 2)),
                );
              },
            );
          },
        ),
      ],
    );
  }

  /// 平台策略: APP本地生效，无需网关参与
  Widget _buildPlatformStrategyGroup(BuildContext context, ParkingProvider provider, PolicyConfig policy) {
    final enabled = policy.otaEnabled;
    return _buildGroup(
      icon: Icons.cloud_outlined,
      title: '平台策略',
      description: 'APP本地生效，无需网关参与',
      rows: [
        _buildDurationRow(
          context: context,
          title: '派单等待时间',
          subtitle: '通知车主后，该时间内未挪车将自动派单（纯APP本地生效）',
          totalSeconds: policy.dispatchWaitSec,
          onChanged: (totalSeconds) {
            provider.updatePolicy(policy.copyWith(dispatchWaitSec: totalSeconds));
          },
        ),
        _buildStepperRow(
          title: '数据刷新间隔',
          subtitle: '每隔该秒数自动拉取一次平台数据',
          value: policy.refreshSec,
          unit: '秒',
          min: 1,
          max: 60,
          onChanged: (v) => provider.updatePolicy(policy.copyWith(refreshSec: v)),
        ),
        _buildSwitchRow(
          title: 'OTA 自动检测',
          subtitle: enabled ? '开启后进入前台自动检测并弹窗提示' : '关闭后不再自动检测升级任务',
          value: enabled,
          onChanged: (v) => provider.updatePolicy(policy.copyWith(otaEnabled: v)),
        ),
        if (enabled) ...[
          _buildStepperRow(
            title: 'OTA 检测间隔',
            subtitle: '每轮检测中相邻两次查询的间隔',
            value: policy.otaIntervalSec,
            unit: '秒',
            min: 1,
            max: 120,
            enabled: enabled,
            onChanged: (v) => provider.updatePolicy(policy.copyWith(otaIntervalSec: v)),
          ),
          _buildStepperRow(
            title: 'OTA 每轮检测次数',
            subtitle: '进入前台立即查 1 次，再加后续次数',
            value: policy.otaCheckCount,
            unit: '次',
            min: 1,
            max: 30,
            enabled: enabled,
            onChanged: (v) => provider.updatePolicy(policy.copyWith(otaCheckCount: v)),
          ),
        ],
      ],
    );
  }

  Widget _buildResetButton(BuildContext context, ParkingProvider provider) {
    return SizedBox(
      width: double.infinity,
      height: 48,
      child: OutlinedButton(
        onPressed: () => provider.updatePolicy(PolicyConfig.defaults),
        style: OutlinedButton.styleFrom(
          foregroundColor: AppColors.textSecondary,
          side: BorderSide(color: AppColors.textSecondary.withValues(alpha: 0.4)),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(24)),
        ),
        child: const Text(
          '恢复默认设置',
          style: TextStyle(fontSize: 14, fontWeight: FontWeight.w500),
        ),
      ),
    );
  }

  /* ==================== 通用构件 ==================== */

  /// 组卡片: 图标 + 标题 + 描述 + 行列表 (行间用左侧缩进的横线切割).
  Widget _buildGroup({
    required IconData icon,
    required String title,
    required String description,
    required List<Widget> rows,
  }) {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(10, 4, 10, 4),
            child: Row(
              children: [
                Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: AppColors.primary.withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(9),
                  ),
                  child: Icon(icon, size: 18, color: AppColors.primary),
                ),
                const SizedBox(width: 10),
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      title,
                      style: const TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                        color: AppColors.textPrimary,
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      description,
                      style: const TextStyle(
                        fontSize: 11,
                        color: AppColors.textSecondary,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(height: 6),
          for (var i = 0; i < rows.length; i++) ...[
            if (i > 0)
              Padding(
                padding: const EdgeInsets.only(left: 16, right: 8),
                child: Container(
                  height: 1,
                  color: AppColors.textSecondary.withValues(alpha: 0.12),
                ),
              ),
            rows[i],
          ],
        ],
      ),
    );
  }

  /// 数值行: 标题/描述在左, 步进器( − 数值单位 + )在右.
  Widget _buildStepperRow({
    required String title,
    required String subtitle,
    required int value,
    required String unit,
    required int min,
    required int max,
    required ValueChanged<int> onChanged,
    bool enabled = true,
  }) {
    final opacity = enabled ? 1.0 : 0.4;
    return Opacity(
      opacity: opacity,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 12),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: const TextStyle(
                      fontSize: 14,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    subtitle,
                    style: const TextStyle(
                      fontSize: 11,
                      color: AppColors.textSecondary,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(width: 12),
            _buildStepper(value, unit, min, max, enabled, onChanged),
          ],
        ),
      ),
    );
  }

  /// ⭐ 数值输入行: 标题/描述在左, 输入框+单位在右. 回车/失焦时触发 onSubmitted.
  Widget _buildInputRow({
    required BuildContext context,
    required String title,
    required String subtitle,
    required int value,
    required String unit,
    required int min,
    required int max,
    required ValueChanged<int> onSubmitted,
    bool enabled = true,
  }) {
    final opacity = enabled ? 1.0 : 0.4;
    final controller = TextEditingController(text: '$value');
    final focusNode = FocusNode();
    return Opacity(
      opacity: opacity,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 12),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: const TextStyle(
                      fontSize: 14,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                  const SizedBox(height: 3),
                  Text(
                    subtitle,
                    style: const TextStyle(
                      fontSize: 11,
                      color: AppColors.textSecondary,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(width: 12),
            SizedBox(
              width: 100,
              height: 36,
              child: TextField(
                controller: controller,
                focusNode: focusNode,
                enabled: enabled,
                textAlign: TextAlign.center,
                textAlignVertical: TextAlignVertical.center,
                keyboardType: TextInputType.number,
                decoration: InputDecoration(
                  isDense: true,
                  contentPadding: EdgeInsets.zero,
                  suffixText: unit,
                  suffixStyle: TextStyle(
                    fontSize: 12,
                    color: enabled ? AppColors.textSecondary : AppColors.textSecondary.withValues(alpha: 0.5),
                  ),
                  enabledBorder: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(8),
                    borderSide: BorderSide(
                      color: AppColors.textSecondary.withValues(alpha: 0.3),
                      width: 0.5,
                    ),
                  ),
                  focusedBorder: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(8),
                    borderSide: BorderSide(
                      color: AppColors.primary,
                      width: 1.0,
                    ),
                  ),
                  disabledBorder: OutlineInputBorder(
                    borderRadius: BorderRadius.circular(8),
                    borderSide: BorderSide(
                      color: AppColors.textSecondary.withValues(alpha: 0.2),
                      width: 0.5,
                    ),
                  ),
                ),
                style: TextStyle(
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                  color: enabled ? AppColors.textPrimary : AppColors.textSecondary,
                ),
                onSubmitted: (text) {
                  final parsed = int.tryParse(text.trim());
                  if (parsed == null) {
                    if (context.mounted) {
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text('请输入 $min-$max 之间的整数'), backgroundColor: Colors.orange, duration: Duration(seconds: 2)),
                      );
                    }
                    controller.text = '$value';  /* 恢复原值 */
                    return;
                  }
                  final clamped = parsed.clamp(min, max);
                  if (clamped != parsed) {
                    if (context.mounted) {
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(content: Text('已限制到合法范围 $min-$max'), backgroundColor: Colors.orange, duration: Duration(seconds: 2)),
                      );
                    }
                    controller.text = '$clamped';
                  }
                  onSubmitted(clamped);
                },
              ),
            ),
          ],
        ),
      ),
    );
  }

  /// 开关行: 标题/描述在左, Switch 在右.
  Widget _buildSwitchRow({
    required String title,
    required String subtitle,
    required bool value,
    required ValueChanged<bool> onChanged,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.w500,
                    color: AppColors.textPrimary,
                  ),
                ),
                const SizedBox(height: 3),
                Text(
                  subtitle,
                  style: const TextStyle(
                    fontSize: 11,
                    color: AppColors.textSecondary,
                  ),
                ),
              ],
            ),
          ),
          Switch(
            value: value,
            activeColor: AppColors.primary,
            inactiveThumbColor: AppColors.textSecondary.withValues(alpha: 0.5),
            inactiveTrackColor: AppColors.textSecondary.withValues(alpha: 0.15),
            onChanged: onChanged,
          ),
        ],
      ),
    );
  }

  /// 步进器: − 数值单位 + (数值位固定宽度, 增减不跳动).
  Widget _buildStepper(
    int value,
    String unit,
    int min,
    int max,
    bool enabled,
    ValueChanged<int> onChanged,
  ) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        _stepButton(
          icon: Icons.remove,
          onTap: enabled && value > min ? () => onChanged(value - 1) : null,
        ),
        SizedBox(
          width: 60,
          child: Text(
            '$value$unit',
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: enabled ? AppColors.textPrimary : AppColors.textSecondary,
            ),
          ),
        ),
        _stepButton(
          icon: Icons.add,
          onTap: enabled && value < max ? () => onChanged(value + 1) : null,
        ),
      ],
    );
  }

  Widget _stepButton({required IconData icon, VoidCallback? onTap}) {
    final enabled = onTap != null;
    final color = enabled
        ? AppColors.textPrimary.withValues(alpha: 0.7)
        : AppColors.textSecondary.withValues(alpha: 0.25);
    return Material(
      color: enabled
          ? AppColors.textPrimary.withValues(alpha: 0.05)
          : Colors.transparent,
      shape: const CircleBorder(),
      child: InkWell(
        onTap: onTap,
        customBorder: const CircleBorder(),
        child: SizedBox(
          width: 32,
          height: 32,
          child: Icon(icon, size: 20, color: color),
        ),
      ),
    );
  }

  /* ==================== ⭐ 时长选择器 (时/分/秒滚轮) ==================== */

  /// 格式化秒数为易读文本 (如 "1小时30分0秒" 或 "15分30秒")
  String _formatDuration(int totalSeconds) {
    final days = totalSeconds ~/ 86400;
    final hours = (totalSeconds % 86400) ~/ 3600;
    final minutes = (totalSeconds % 3600) ~/ 60;
    final seconds = totalSeconds % 60;
    if (days > 0) {
      return '$days天${hours.toString().padLeft(2, '0')}时${minutes.toString().padLeft(2, '0')}分';
    } else if (hours > 0) {
      return '$hours时${minutes.toString().padLeft(2, '0')}分${seconds.toString().padLeft(2, '0')}秒';
    } else {
      return '$minutes分${seconds.toString().padLeft(2, '0')}秒';
    }
  }

  /// 构建时长选择行: 显示格式化时间, 点击弹出滚轮选择器
  Widget _buildDurationRow({
    required BuildContext context,
    required String title,
    required String subtitle,
    required int totalSeconds,
    required ValueChanged<int> onChanged,
    bool enabled = true,  /* ⭐ 是否可点击 */
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 12),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Text(
                      title,
                      style: TextStyle(
                        fontSize: 14,
                        fontWeight: FontWeight.w500,
                        color: enabled ? AppColors.textPrimary : AppColors.textSecondary,
                      ),
                    ),
                    if (!enabled) ...[
                      const SizedBox(width: 8),
                      Container(
                        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                        decoration: BoxDecoration(
                          color: Colors.orange.withValues(alpha: 0.15),
                          borderRadius: BorderRadius.circular(4),
                        ),
                        child: const Row(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Icon(Icons.wifi_off, size: 12, color: Colors.orange),
                            SizedBox(width: 3),
                            Text('网关离线', style: TextStyle(fontSize: 10, color: Colors.orange)),
                          ],
                        ),
                      ),
                    ],
                  ],
                ),
                const SizedBox(height: 3),
                Text(
                  subtitle,
                  style: TextStyle(
                    fontSize: 11,
                    color: enabled ? AppColors.textSecondary : AppColors.textSecondary.withValues(alpha: 0.6),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          /* ⭐ 点击区域: 显示当前格式化时间 + 下拉箭头 */
          GestureDetector(
            onTap: enabled ? () => _showDurationPicker(context, totalSeconds, onChanged) : null,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              decoration: BoxDecoration(
                color: enabled ? AppColors.primary.withValues(alpha: 0.1) : Colors.grey.withValues(alpha: 0.1),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(
                  color: enabled ? AppColors.primary.withValues(alpha: 0.3) : Colors.grey.withValues(alpha: 0.3),
                  width: 0.5,
                ),
              ),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    _formatDuration(totalSeconds),
                    style: TextStyle(
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      color: enabled ? AppColors.primary : Colors.grey,
                    ),
                  ),
                  const SizedBox(width: 4),
                  Icon(
                    Icons.access_time,
                    size: 18,
                    color: enabled ? AppColors.primary : Colors.grey,
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// 弹出底部模态框, 让用户通过滚轮选择 天/时/分/秒
  Future<void> _showDurationPicker(
    BuildContext context,
    int initialSeconds,
    ValueChanged<int> onChanged,
  ) async {
    int tempDays = initialSeconds ~/ 86400;
    int tempHours = (initialSeconds % 86400) ~/ 3600;
    int tempMinutes = (initialSeconds % 3600) ~/ 60;
    int tempSeconds = initialSeconds % 60;

    await showModalBottomSheet(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (context) {
        return Container(
          height: 340,
          decoration: const BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
          ),
          child: Column(
            children: [
              /* 头部: 标题 + 取消/确定按钮 */
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
                child: Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    TextButton(
                      onPressed: () => Navigator.pop(context),
                      child: const Text('取消', style: TextStyle(color: AppColors.textSecondary)),
                    ),
                    const Text(
                      '选择判定时长',
                      style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
                    ),
                    TextButton(
                      onPressed: () {
                        final total = tempDays * 86400 + tempHours * 3600 + tempMinutes * 60 + tempSeconds;
                        /* ⭐ 确保最小值为 5 秒 */
                        final clamped = total < 5 ? 5 : total;
                        onChanged(clamped);
                        Navigator.pop(context);
                      },
                      child: Text('确定', style: TextStyle(color: AppColors.primary, fontWeight: FontWeight.w600)),
                    ),
                  ],
                ),
              ),
              const Divider(height: 1),
              /* 滚轮选择器: 天 / 时 / 分 / 秒 */
              Expanded(
                child: Row(
                  children: [
                    /* 天滚轮 (0-30) */
                    Expanded(
                      child: CupertinoPicker(
                        itemExtent: 40,
                        onSelectedItemChanged: (v) => tempDays = v,
                        scrollController: FixedExtentScrollController(initialItem: tempDays),
                        children: List.generate(31, (i) => Center(child: Text('$i 天'))),
                      ),
                    ),
                    /* 小时滚轮 (0-23) */
                    Expanded(
                      child: CupertinoPicker(
                        itemExtent: 40,
                        onSelectedItemChanged: (v) => tempHours = v,
                        scrollController: FixedExtentScrollController(initialItem: tempHours),
                        children: List.generate(24, (i) => Center(child: Text('$i 时'))),
                      ),
                    ),
                    /* 分钟滚轮 (0-59) */
                    Expanded(
                      child: CupertinoPicker(
                        itemExtent: 40,
                        onSelectedItemChanged: (v) => tempMinutes = v,
                        scrollController: FixedExtentScrollController(initialItem: tempMinutes),
                        children: List.generate(60, (i) => Center(child: Text('$i 分'))),
                      ),
                    ),
                    /* 秒滚轮 (0-59) */
                    Expanded(
                      child: CupertinoPicker(
                        itemExtent: 40,
                        onSelectedItemChanged: (v) => tempSeconds = v,
                        scrollController: FixedExtentScrollController(initialItem: tempSeconds),
                        children: List.generate(60, (i) => Center(child: Text('$i 秒'))),
                      ),
                    ),
                  ],
                ),
              ),
              /* 快捷预设按钮 */
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
                child: Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  alignment: WrapAlignment.center,
                  children: [
                    _buildQuickPreset('5秒', 5, onChanged, context),
                    _buildQuickPreset('30秒', 30, onChanged, context),
                    _buildQuickPreset('1分钟', 60, onChanged, context),
                    _buildQuickPreset('5分钟', 300, onChanged, context),
                    _buildQuickPreset('1小时', 3600, onChanged, context),
                    _buildQuickPreset('1天', 86400, onChanged, context),
                    _buildQuickPreset('3天', 259200, onChanged, context),
                    _buildQuickPreset('7天', 604800, onChanged, context),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }

  /// 快捷预设按钮 (如 "5秒", "30秒")
  Widget _buildQuickPreset(String label, int seconds, ValueChanged<int> onChanged, BuildContext context) {
    return GestureDetector(
      onTap: () {
        onChanged(seconds);
        Navigator.pop(context);
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
        decoration: BoxDecoration(
          color: AppColors.primary.withValues(alpha: 0.08),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: AppColors.primary.withValues(alpha: 0.2)),
        ),
        child: Text(
          label,
          style: TextStyle(fontSize: 12, color: AppColors.primary, fontWeight: FontWeight.w500),
        ),
      ),
    );
  }
}
