import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/custom_app_bar.dart';

/// 固件升级页 (全网一键升级, 方案A: 先跑通单节点 Park001).
///
/// 流程: 查任务 → App 确认(下发 OtaAllow=1) → 网关执行 → 轮询任务状态 →
///       完成后复位(OtaAllow=0).
/// 后续扩展方案C(按节点遍历): 把目标设备从单个 Park001 换成节点列表即可.
class FirmwareUpgradePage extends StatefulWidget {
  const FirmwareUpgradePage({super.key});

  @override
  State<FirmwareUpgradePage> createState() => _FirmwareUpgradePageState();
}

class _FirmwareUpgradePageState extends State<FirmwareUpgradePage> {
  static const String _deviceName = 'Park001';
  static const String _defaultVersion = 'v2.321'; // 读不到版本时的兜底(NODE_FW_VERSION)

  ParkingProvider get _provider => context.read<ParkingProvider>();

  bool _loading = true;
  String? _currentVersion;
  Map<String, dynamic>? _task;
  String? _tid;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _task = null;
      _tid = null;
    });
    // 先读节点当前版本(平台记录的 s_version), 再以该版本检测升级任务
    final version = await _provider.getOtaNodeVersion(_deviceName) ?? _defaultVersion;
    final task = await _provider.getOtaTask(_deviceName, version: version);
    if (!mounted) return;
    // 同步任务到全局 OTA 状态机, 与升级弹窗共用同一份状态/进度
    _provider.syncOtaTask(task, version);
    setState(() {
      _loading = false;
      _currentVersion = version;
      _task = task;
      _tid = task?['tid']?.toString();
    });
  }

  /// 确认升级: 复用全局 OTA 状态机(与弹窗一致), 下发 OtaAllow=1 并轮询进度.
  Future<void> _confirmUpgrade() async {
    final tid = _tid;
    if (tid == null) return;
    final ok = await _provider.startManualOta(tid, _task?['target']?.toString() ?? '');
    if (!mounted) return;
    if (!ok) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('确认指令下发失败，请重试')),
      );
    }
  }

  Color _statusColor(int? status) {
    switch (status) {
      case 2:
      case 3: return AppColors.warning;
      case 4: return AppColors.success;
      case 5:
      case 6: return AppColors.danger;
      default: return AppColors.warning; // 待升级/进行中(平台status不可靠) 用警示色
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: const CustomAppBar(title: '固件升级', showBackButton: true),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _buildTargetCard(),
            const SizedBox(height: AppDims.gapCard),
            _buildVersionCard(),
            const SizedBox(height: AppDims.gapCard),
            _buildStatusCard(),
            const SizedBox(height: AppDims.gapCard),
            _buildActionArea(),
            const SizedBox(height: 16),
            Text(
              '说明：控制台创建升级任务后，此处确认下发，网关收到确认后才会执行升级；完成后自动复位。',
              style: TextStyle(
                fontSize: 12,
                color: AppColors.textSecondary.withValues(alpha: 0.8),
              ),
            ),
          ],
        ),
      ),
    );
  }

  /* ---- 升级目标 ---- */
  Widget _buildTargetCard() {
    return CardContainer(
      padding: const EdgeInsets.all(AppDims.paddingCard),
      child: Row(
        children: [
          Container(
            width: 44,
            height: 44,
            decoration: BoxDecoration(
              color: AppColors.primaryLight,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.memory_outlined, color: AppColors.primary, size: 24),
          ),
          const SizedBox(width: 14),
          const Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  '升级目标设备',
                  style: TextStyle(
                    fontSize: 13,
                    color: AppColors.textSecondary,
                  ),
                ),
                SizedBox(height: 4),
                Text(
                  _deviceName,
                  style: TextStyle(
                    fontSize: 17,
                    fontWeight: FontWeight.w600,
                    color: AppColors.textPrimary,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  /* ---- 版本信息 ---- */
  Widget _buildVersionCard() {
    // 升级成功后: 已是最新版本, 不再显示旧版本/目标版本
    final status = context.watch<ParkingProvider>().otaStatus;
    if (status == 4) {
      return const CardContainer(
        padding: EdgeInsets.all(AppDims.paddingCard),
        child: Row(
          children: [
            Icon(Icons.check_circle, color: AppColors.success, size: 22),
            SizedBox(width: 10),
            Text(
              '当前已是最新版本',
              style: TextStyle(
                fontSize: 15,
                fontWeight: FontWeight.w600,
                color: AppColors.textPrimary,
              ),
            ),
          ],
        ),
      );
    }
    return CardContainer(
      padding: const EdgeInsets.all(AppDims.paddingCard),
      child: Column(
        children: [
          _buildVersionRow('当前版本', _currentVersion ?? '—'),
          const SizedBox(height: 12),
          Container(height: 1, color: AppColors.textSecondary.withValues(alpha: 0.15)),
          const SizedBox(height: 12),
          _buildVersionRow('目标版本', _task?['target']?.toString() ?? '—'),
        ],
      ),
    );
  }

  Widget _buildVersionRow(String label, String value) {
    return Row(
      children: [
        Text(
          label,
          style: const TextStyle(fontSize: 14, color: AppColors.textSecondary),
        ),
        const Spacer(),
        Text(
          value,
          style: const TextStyle(
            fontSize: 15,
            fontWeight: FontWeight.w600,
            color: AppColors.textPrimary,
          ),
        ),
      ],
    );
  }

  /* ---- 任务状态 ---- */
  Widget _buildStatusCard() {
    if (_loading) {
      return const CardContainer(
        padding: EdgeInsets.symmetric(vertical: 32),
        child: Center(child: CircularProgressIndicator()),
      );
    }

    // 与升级弹窗共用全局 OTA 状态机, 同一份状态/进度;
    // 进度条在升级中 或 完成后的1秒停留(让用户看清100%)时显示
    final p = context.watch<ParkingProvider>();
    final status = p.otaStatus;
    final text = status != null ? p.otaStatusText : '暂无升级任务';
    final color = _statusColor(status);
    final upgrading = p.otaShowProgress;

    return CardContainer(
      padding: const EdgeInsets.all(AppDims.paddingCard),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            '任务状态',
            style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Icon(Icons.circle, size: 10, color: color),
              const SizedBox(width: 8),
              Text(
                text,
                style: TextStyle(
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                  color: color,
                ),
              ),
              const Spacer(),
              if (p.otaConfirming)
                const SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2),
                )
              else
                IconButton(
                  icon: const Icon(Icons.refresh, size: 20, color: AppColors.textSecondary),
                  tooltip: '刷新',
                  onPressed: _loading ? null : _load,
                ),
            ],
          ),
          if (upgrading) ...[
            const SizedBox(height: 12),
            Row(
              children: [
                Text(
                  '升级进度',
                  style: const TextStyle(
                    fontSize: 13,
                    color: AppColors.textSecondary,
                  ),
                ),
                const Spacer(),
                Text(
                  '${p.otaStep}%',
                  style: TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.w700,
                    color: color,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            ClipRRect(
              borderRadius: BorderRadius.circular(4),
              child: LinearProgressIndicator(
                value: p.otaStep / 100,
                minHeight: 8,
                backgroundColor: AppColors.primary.withValues(alpha: 0.15),
                color: color,
              ),
            ),
          ],
        ],
      ),
    );
  }

  /* ---- 操作区 ---- */
  Widget _buildActionArea() {
    final p = context.watch<ParkingProvider>();
    final hasTask = _task != null;
    final busy = p.otaConfirming;
    final done = p.otaStatus == 4;

    return SizedBox(
      width: double.infinity,
      child: FilledButton(
        style: FilledButton.styleFrom(
          backgroundColor: done ? AppColors.textSecondary : AppColors.primary,
          padding: const EdgeInsets.symmetric(vertical: 14),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(AppDims.radiusMedium),
          ),
        ),
        onPressed: (hasTask && !busy && !done && !_loading) ? _confirmUpgrade : null,
        child: Text(
          done ? '升级完成' : (busy ? '已下发确认，等待网关执行…' : '确认升级'),
          style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
        ),
      ),
    );
  }
}
