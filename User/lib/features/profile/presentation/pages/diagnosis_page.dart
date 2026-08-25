import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';

/// 单条诊断问题 (等级: critical 严重 / warning 警告 / info 提示)
class DiagnosisIssue {
  final String level;
  final String title;
  final String detail;
  final String suggestion;

  DiagnosisIssue({
    required this.level,
    required this.title,
    required this.detail,
    required this.suggestion,
  });

  bool get isCritical => level == 'critical';
  bool get isWarning => level == 'warning';

  Color get color => isCritical
      ? AppColors.danger
      : isWarning
          ? AppColors.warning
          : AppColors.primary;

  IconData get icon => isCritical
      ? Icons.error_outline
      : isWarning
          ? Icons.warning_amber_rounded
          : Icons.info_outline;

  String get levelText => isCritical ? '严重' : isWarning ? '警告' : '提示';
}

/// 诊断阶段: idle=待命 / diagnosing=扫描中 / done=已完成
enum _DiagPhase { idle, diagnosing, done }

/// 故障诊断页 (极简风格: 手动触发 + 扫描动画 + 结果列表)
///
/// 数据全部来自 ParkingProvider.spots (与车位页同一数据源):
/// - 设备离线: 直接可见
/// - 传感器数据矛盾: 地磁与超声波判定不一致
/// - 长时间占用 / 数据更新延迟: 业务与通信层面的提示
/// (电池/信号未真实采集, 不纳入诊断)
class DiagnosisPage extends StatefulWidget {
  const DiagnosisPage({super.key});

  @override
  State<DiagnosisPage> createState() => _DiagnosisPageState();
}

class _DiagnosisPageState extends State<DiagnosisPage>
    with SingleTickerProviderStateMixin {
  /// 超声波判定"有车"的距离阈值(cm), 与节点固件 DIST_THRESHOLD_CM=10 保持一致.
  static const int _usCarThresholdCm = 10;

  /// 每台设备的扫描间隔(ms), 控制扫描节奏.
  static const int _scanStepMs = 300;

  _DiagPhase _phase = _DiagPhase.idle;
  List<SpotModel> _snapshot = []; // 扫描时的数据快照
  int _checkIndex = 0; // 已检查设备数
  List<DiagnosisIssue> _issues = []; // 诊断结果

  late final AnimationController _scanController; // 扫描旋转动画

  @override
  void initState() {
    super.initState();
    _scanController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1200),
    )..repeat();
  }

  @override
  void dispose() {
    _scanController.dispose();
    super.dispose();
  }

  /// 手动触发诊断: 逐台推进设备扫描, 每查到一条问题立即实时上屏, 完成后汇总.
  Future<void> _startDiagnose() async {
    final spots = context.read<ParkingProvider>().spots;
    setState(() {
      _phase = _DiagPhase.diagnosing;
      _checkIndex = 0;
      _issues = [];
      _snapshot = spots;
    });
    for (var i = 0; i < _snapshot.length; i++) {
      await Future.delayed(const Duration(milliseconds: _scanStepMs));
      if (!mounted) return;
      // 单台立即诊断: 结果实时追加到列表, 逐条上屏
      final found = _diagnoseSpot(_snapshot[i]);
      setState(() {
        _checkIndex = i + 1;
        _issues.addAll(found);
      });
    }
    if (!mounted) return;
    setState(() => _phase = _DiagPhase.done);
  }

  /// 诊断单台设备, 按预设规则产出问题.
  ///
  /// 传感器异常 = 双传感器数据互相矛盾 (与节点固件的双传感器 AND 判定对应):
  /// - 地磁感应到车(1), 但超声波距离很远(>=阈值判无车) → 超声波可能被遮挡/故障
  /// - 超声波距离很近(<阈值判有车), 但地磁未感应(0)   → 地磁可能异常/受干扰
  /// 注意: 仅凭长时间占用(僵尸车)不构成传感器异常.
  List<DiagnosisIssue> _diagnoseSpot(SpotModel spot) {
    final issues = <DiagnosisIssue>[];
    if (spot.isOffline) {
      issues.add(DiagnosisIssue(
        level: 'critical',
        title: '${spot.id} 设备离线',
        detail: '设备当前处于离线状态，无法上报车位数据',
        suggestion: '检查设备供电与网络连接，必要时现场排查',
      ));
    }

    // 传感器矛盾 1: 地磁感应到车, 但超声波距离远 → 超声波侧异常
    if (spot.geoMagnetic == 1 && spot.ultrasonic >= _usCarThresholdCm) {
      issues.add(DiagnosisIssue(
        level: 'warning',
        title: '${spot.id} 传感器数据矛盾',
        detail: '地磁感应到车辆，但超声波距离 ${spot.ultrasonic}cm 偏远，两传感器判定不一致',
        suggestion: '超声波探头可能被异物遮挡或损坏，建议现场检查',
      ));
    }
    // 传感器矛盾 2: 超声波距离很近(判有车), 但地磁未感应 → 地磁侧异常
    if (spot.geoMagnetic == 0 &&
        spot.ultrasonic > 0 &&
        spot.ultrasonic < _usCarThresholdCm) {
      issues.add(DiagnosisIssue(
        level: 'warning',
        title: '${spot.id} 传感器数据矛盾',
        detail: '超声波距离 ${spot.ultrasonic}cm 很近，但地磁未感应到车辆，两传感器判定不一致',
        suggestion: '地磁传感器可能异常或受磁场干扰，建议现场检查',
      ));
    }

    // 僵尸车为业务状态(长期占用), 非传感器故障: 仅作运营提醒
    if (spot.isZombie) {
      issues.add(DiagnosisIssue(
        level: 'info',
        title: '${spot.id} 长时间占用',
        detail: '该车位已连续占用 ${spot.occupiedHours} 小时，传感器数据正常',
        suggestion: '建议通知车主挪车，必要时派单处理',
      ));
    }

    final lastUpdated = spot.lastUpdated;
    if (lastUpdated != null && lastUpdated.isNotEmpty) {
      final updatedAt = DateTime.tryParse(lastUpdated);
      if (updatedAt != null &&
          DateTime.now().difference(updatedAt).inMinutes > 60) {
        issues.add(DiagnosisIssue(
          level: 'info',
          title: '${spot.id} 数据更新延迟',
          detail: '设备已超过 60 分钟未上报新数据',
          suggestion: '检查设备通信链路与网关状态',
        ));
      }
    }
    return issues;
  }

  @override
  Widget build(BuildContext context) {
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
          '故障诊断',
          style: TextStyle(
            color: AppColors.textPrimary,
            fontSize: 17,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 20),
          child: Column(
            children: [
              const SizedBox(height: 8),
              Expanded(
                child: switch (_phase) {
                  _DiagPhase.idle => _buildIdle(),
                  _DiagPhase.diagnosing => _buildDiagnosing(),
                  _DiagPhase.done => _buildResult(),
                },
              ),
              _buildActionButton(),
              const SizedBox(height: 24),
            ],
          ),
        ),
      ),
    );
  }

  /// 待命态: 提示用户手动开始诊断.
  Widget _buildIdle() {
    return Align(
      // 黄金分割: 内容中心置于内容区距顶 38.2% 处 (视觉重心居中偏上, 自适应屏高)
      alignment: const Alignment(0, -0.236),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 120,
            height: 120,
            decoration: BoxDecoration(
              color: AppColors.primary.withValues(alpha: 0.06),
              shape: BoxShape.circle,
            ),
            child: const Icon(
              Icons.sensors,
              size: 56,
              color: AppColors.primary,
            ),
          ),
          const SizedBox(height: 24),
          const Text(
            '一键诊断全车位设备',
            style: TextStyle(
              fontSize: 18,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            '检查设备离线、传感器数据矛盾、\n数据更新延迟等情况',
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 13,
              height: 1.6,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      ),
    );
  }

  /// 扫描态: 紧凑头部(旋转动画+进度) + 已发现问题实时列表.
  Widget _buildDiagnosing() {
    final total = _snapshot.length;
    final current = _checkIndex > 0 && _checkIndex <= total
        ? _snapshot[_checkIndex - 1].id
        : '';
    final progress = total > 0 ? _checkIndex / total : 0.0;

    return Column(
      children: [
        const SizedBox(height: 8),
        // 紧凑扫描头部
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          decoration: BoxDecoration(
            color: AppColors.surface,
            borderRadius: BorderRadius.circular(12),
            boxShadow: [
              BoxShadow(
                color: Colors.black.withValues(alpha: 0.05),
                blurRadius: 12,
                offset: const Offset(0, 3),
              ),
            ],
          ),
          child: Row(
            children: [
              RotationTransition(
                turns: _scanController,
                child: Container(
                  width: 44,
                  height: 44,
                  decoration: BoxDecoration(
                    color: AppColors.primary.withValues(alpha: 0.08),
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: const Icon(
                    Icons.sensors,
                    size: 24,
                    color: AppColors.primary,
                  ),
                ),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        const Text(
                          '正在诊断',
                          style: TextStyle(
                            fontSize: 15,
                            fontWeight: FontWeight.w600,
                            color: AppColors.textPrimary,
                          ),
                        ),
                        const Spacer(),
                        Text(
                          '第 $_checkIndex/$total 台 · $current',
                          style: const TextStyle(
                            fontSize: 12,
                            color: AppColors.textSecondary,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 10),
                    ClipRRect(
                      borderRadius: BorderRadius.circular(4),
                      child: LinearProgressIndicator(
                        value: progress,
                        minHeight: 6,
                        backgroundColor:
                            AppColors.primary.withValues(alpha: 0.12),
                        color: AppColors.primary,
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
        const SizedBox(height: 16),
        // 已发现问题: 逐条实时上屏
        Expanded(
          child: _issues.isEmpty
              ? Center(
                  child: Text(
                    '正在逐台检查设备...',
                    style: TextStyle(
                      fontSize: 13,
                      color: AppColors.textSecondary.withValues(alpha: 0.7),
                    ),
                  ),
                )
              : ListView.separated(
                  padding: const EdgeInsets.only(bottom: 16),
                  itemCount: _issues.length,
                  separatorBuilder: (_, __) => const SizedBox(height: 12),
                  itemBuilder: (context, index) =>
                      _buildIssueItem(_issues[index]),
                ),
        ),
      ],
    );
  }

  /// 结果态: 摘要卡片 + 问题列表.
  Widget _buildResult() {
    final issues = _issues;
    final total = _snapshot.length;

    return Column(
      children: [
        _buildSummary(issues, total),
        const SizedBox(height: 24),
        Expanded(
          child: issues.isEmpty
              ? _buildNoIssue()
              : ListView.separated(
                  padding: const EdgeInsets.only(bottom: 16),
                  itemCount: issues.length,
                  separatorBuilder: (_, __) => const SizedBox(height: 12),
                  itemBuilder: (context, index) =>
                      _buildIssueItem(issues[index]),
                ),
        ),
      ],
    );
  }

  /// 顶部摘要卡片: 是否有问题 + 检查设备总数.
  Widget _buildSummary(List<DiagnosisIssue> issues, int total) {
    final hasIssue = issues.isNotEmpty;
    final color = hasIssue
        ? (issues.any((i) => i.isCritical) ? AppColors.danger : AppColors.warning)
        : AppColors.success;
    final icon = hasIssue ? Icons.report_problem_outlined : Icons.check_circle_outline;
    final title = hasIssue ? '发现 ${issues.length} 个问题' : '系统运行正常';

    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(vertical: 28),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(16),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.05),
            blurRadius: 16,
            offset: const Offset(0, 4),
          ),
        ],
      ),
      child: Column(
        children: [
          Icon(icon, size: 52, color: color),
          const SizedBox(height: 12),
          Text(
            title,
            style: const TextStyle(
              fontSize: 18,
              fontWeight: FontWeight.w600,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            '共检查 $total 台设备',
            style: const TextStyle(
              fontSize: 13,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      ),
    );
  }

  /// 无问题时列表区的占位提示.
  Widget _buildNoIssue() {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            Icons.task_alt,
            size: 56,
            color: AppColors.success.withValues(alpha: 0.4),
          ),
          const SizedBox(height: 12),
          const Text(
            '所有设备运行正常',
            style: TextStyle(
              fontSize: 14,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      ),
    );
  }

  /// 单条问题卡片: 等级图标 + 标题/详情/建议 + 等级标签.
  Widget _buildIssueItem(DiagnosisIssue issue) {
    final color = issue.color;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withValues(alpha: 0.05),
            blurRadius: 12,
            offset: const Offset(0, 3),
          ),
        ],
      ),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: color.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(10),
            ),
            child: Icon(issue.icon, color: color, size: 22),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Expanded(
                      child: Text(
                        issue.title,
                        style: const TextStyle(
                          fontSize: 15,
                          fontWeight: FontWeight.w600,
                          color: AppColors.textPrimary,
                        ),
                      ),
                    ),
                    const SizedBox(width: 8),
                    Container(
                      padding: const EdgeInsets.symmetric(
                        horizontal: 8,
                        vertical: 2,
                      ),
                      decoration: BoxDecoration(
                        color: color.withValues(alpha: 0.12),
                        borderRadius: BorderRadius.circular(6),
                      ),
                      child: Text(
                        issue.levelText,
                        style: TextStyle(
                          fontSize: 11,
                          fontWeight: FontWeight.w600,
                          color: color,
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 6),
                Text(
                  issue.detail,
                  style: const TextStyle(
                    fontSize: 12,
                    height: 1.4,
                    color: AppColors.textSecondary,
                  ),
                ),
                const SizedBox(height: 6),
                Text(
                  '建议：${issue.suggestion}',
                  style: const TextStyle(
                    fontSize: 12,
                    height: 1.4,
                    color: AppColors.textSecondary,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  /// 底部操作按钮: 随诊断阶段变化 (开始诊断 / 诊断中 / 重新诊断).
  Widget _buildActionButton() {
    final String text;
    final VoidCallback? onPressed;
    switch (_phase) {
      case _DiagPhase.idle:
        text = '开始诊断';
        onPressed = _startDiagnose;
        break;
      case _DiagPhase.diagnosing:
        text = '诊断中...';
        onPressed = null;
        break;
      case _DiagPhase.done:
        text = '重新诊断';
        onPressed = _startDiagnose;
        break;
    }
    return SizedBox(
      width: double.infinity,
      height: 52,
      child: FilledButton(
        onPressed: onPressed,
        style: FilledButton.styleFrom(
          backgroundColor: AppColors.primary,
          disabledBackgroundColor: AppColors.primary.withValues(alpha: 0.5),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(26),
          ),
        ),
        child: Text(
          text,
          style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
        ),
      ),
    );
  }
}
