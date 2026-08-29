import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';

/// 固件升级页 (极简风格: 大环形进度条 + 状态文字 + 操作按钮)
class FirmwareUpgradePage extends StatefulWidget {
  const FirmwareUpgradePage({super.key});

  @override
  State<FirmwareUpgradePage> createState() => _FirmwareUpgradePageState();
}

class _FirmwareUpgradePageState extends State<FirmwareUpgradePage> {
  static const String _deviceName = 'Park001';
  static const String _defaultVersion = 'v2.321';

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

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _task = null;
      _tid = null;
    });
    final version = await _provider.getOtaNodeVersion(_deviceName) ?? _defaultVersion;
    final task = await _provider.getOtaTask(_deviceName, version: version);
    if (!mounted) return;
    _provider.syncOtaTask(task, version);
    setState(() {
      _loading = false;
      _currentVersion = version;
      _task = task;
      _tid = task?['tid']?.toString();
    });
  }

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

  Color _ringColor(int? status) {
    if (_loading) return AppColors.primary.withValues(alpha: 0.3);
    if (status == 4) return AppColors.success;
    if (status == 5 || status == 6) return AppColors.danger;
    if (status == 2 || status == 3) return AppColors.primary;
    return AppColors.primary.withValues(alpha: 0.6);
  }

  String _centerText() {
    if (_loading) return '检查中';
    final p = _provider;
    final status = p.otaStatus;
    final step = p.otaStep;
    
    if (p.otaConfirming) return '$step%';
    if (status == 4) return '100%';
    // 有新版本: 圆环中心大字显示目标版本, 不再显示空文字
    if (_task != null) return _task?['target']?.toString() ?? '';
    return '100%';
  }

  String _subText() {
    if (_loading) return '正在检查更新...';
    final p = _provider;
    final status = p.otaStatus;
    
    if (p.otaConfirming) return '升级中...';
    if (status == 4) return '升级完成';
    if (status == 5 || status == 6) return '升级失败';
    // 有新版本: 不再显示"发现新版本"小字, 信息已在圆环中心大字体现
    if (_task != null) return '';
    return '当前已是最新版本';
  }

  String _buttonText() {
    if (_loading) return '检查中...';
    final p = _provider;
    final status = p.otaStatus;
    
    if (p.otaConfirming) return '升级中';
    if (status == 4) return '完成';
    if (status == 5 || status == 6) return '重试';
    if (_task != null) return '立即更新';
    return '已是最新';
  }

  bool _showButton() {
    if (_loading) return false;
    final p = _provider;
    return !p.otaConfirming;
  }

  /// 是否需要渲染底部操作按钮 (与 _buildActionButton 的返回逻辑保持一致).
  bool _shouldShowButton(ParkingProvider p) {
    if (_loading) return false;
    final status = p.otaStatus;
    if (p.otaConfirming) return true; // 升级中(禁用按钮)
    if (status == 4) return true; // 已完成
    if (status == 5 || status == 6) return true; // 失败
    if (_task != null && status == 1) return true; // 待升级
    return false; // 已是最新
  }

  @override
  Widget build(BuildContext context) {
    final p = context.watch<ParkingProvider>();
    final status = p.otaStatus;
    final ringProgress = _getRingProgress();
    final ringColor = _ringColor(status);

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
          '固件升级',
          style: TextStyle(
            color: AppColors.textPrimary,
            fontSize: 17,
            fontWeight: FontWeight.w600,
          ),
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.info_outline, color: AppColors.textSecondary),
            onPressed: () {
              showDialog(
                context: context,
                builder: (_) => AlertDialog(
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
                  title: const Text('升级说明', style: TextStyle(fontWeight: FontWeight.w600)),
                  content: const Text(
                    '控制台创建升级任务后，需在此处确认下发。网关收到确认指令后才会执行升级，完成后自动复位。\n\n支持 OTA 网关远程升级。',
                    style: TextStyle(height: 1.5),
                  ),
                  actions: [
                    FilledButton(
                      onPressed: () => Navigator.pop(context),
                      style: FilledButton.styleFrom(
                        backgroundColor: AppColors.primary,
                      ),
                      child: const Text('我知道了'),
                    ),
                  ],
                ),
              );
            },
          ),
        ],
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 24),
          child: Column(
            children: [
              // 核心区域: 圆环/logo + 版本信息, 黄金分割定位 (内容中心在内容区距顶 38.2% 处)
              Expanded(
                child: Align(
                  alignment: const Alignment(0, -0.236),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      _buildMainContent(ringProgress, ringColor, p),
                      if (!_loading) ...[
                        const SizedBox(height: 32),
                        _buildVersionInfo(p),
                      ],
                    ],
                  ),
                ),
              ),
              // 底部操作按钮区域, 仅在非加载态且需要显示时渲染
              if (!_loading && _shouldShowButton(p)) ...[
                _buildActionButton(p),
                const SizedBox(height: 24),
              ],
            ],
          ),
        ),
      ),
    );
  }

  double? _getRingProgress() {
    if (_loading) return null;
    final p = _provider;
    final status = p.otaStatus;
    
    if (p.otaConfirming) return p.otaStep / 100.0;
    if (status == 4 || status == 5 || status == 6) return 1.0;
    if (_task != null && status == 1) return 0.0;
    if (_task != null) return p.otaStep / 100.0;
    return 1.0;
  }

  /// 核心内容: 根据状态决定显示环形进度条还是 logo.
  Widget _buildMainContent(double? ringProgress, Color ringColor, ParkingProvider p) {
    final status = p.otaStatus;
    final isUpdating = p.otaConfirming || (status == 2 || status == 3);
    final hasUpdate = _task != null && status == 1;
    final isDoneOrFailed = status == 4 || status == 5 || status == 6;
    
    // 检查中 / 有新版本 / 升级中 / 已完成 / 失败 → 显示环形进度条
    if (_loading || hasUpdate || isUpdating || isDoneOrFailed) {
      return _buildProgressRing(ringProgress, ringColor, p);
    }
    // 无新版本 → 显示 logo
    return _buildNoUpdateLogo();
  }

  /// 无新版本时显示的 logo + 文字组合.
  Widget _buildNoUpdateLogo() {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(
          Icons.check_circle_outline,
          size: 120,
          color: AppColors.textSecondary.withValues(alpha: 0.5),
        ),
        const SizedBox(height: 24),
        const Text(
          '当前已是最新版本',
          style: TextStyle(
            fontSize: 16,
            color: AppColors.textSecondary,
            fontWeight: FontWeight.w500,
          ),
        ),
      ],
    );
  }

  Widget _buildProgressRing(double? progress, Color color, ParkingProvider p) {
    // 浅色全圆轨道: 任何状态(含检查中)都显示完整圆环, 保证视觉始终居中不偏移.
    // 注意: 不确定模式(检查中 progress==null)下 CircularProgressIndicator 不绘制
    // backgroundColor 轨道, 只画一段旋转弧, 单独显示会显得圆环偏一侧; 故在底层
    // 用确定模式(value:0)画一条完整浅色轨道垫底.
    final trackColor = AppColors.textSecondary.withValues(alpha: 0.12);
    return SizedBox(
      width: 240,
      height: 240,
      child: Stack(
        alignment: Alignment.center,
        children: [
          Container(
            width: 240,
            height: 240,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: AppColors.surface,
              boxShadow: [
                BoxShadow(
                  color: AppColors.primary.withValues(alpha: 0.08),
                  blurRadius: 40,
                  offset: const Offset(0, 20),
                ),
              ],
            ),
          ),
          SizedBox(
            width: 210,
            height: 210,
            child: Stack(
              fit: StackFit.expand,
              children: [
                // 底层: 完整浅色轨道(确定模式 value:0 只显示轨道)
                CircularProgressIndicator(
                  value: 0,
                  strokeWidth: 8,
                  backgroundColor: trackColor,
                  valueColor: const AlwaysStoppedAnimation(Colors.transparent),
                ),
                // 上层: 实际进度弧(检查中为旋转动画, 升级中为实时百分比)
                CircularProgressIndicator(
                  value: progress,
                  strokeWidth: 8,
                  valueColor: AlwaysStoppedAnimation<Color>(color),
                ),
              ],
            ),
          ),
          Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(
                _centerText(),
                style: TextStyle(
                  fontSize: p.otaConfirming ? 42 : 48,
                  fontWeight: FontWeight.w300,
                  color: AppColors.textPrimary,
                  height: 1.1,
                ),
              ),
              const SizedBox(height: 4),
              Text(
                _subText(),
                style: TextStyle(
                  fontSize: 14,
                  color: color,
                  fontWeight: FontWeight.w500,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildVersionInfo(ParkingProvider p) {
    if (_loading) {
      return const SizedBox.shrink();
    }
    
    final status = p.otaStatus;
    final hasUpdate = _task != null && status == 1;
    final isSuccess = status == 4;
    
    if (isSuccess) {
      return Column(
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 8),
            decoration: BoxDecoration(
              color: AppColors.success.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(20),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Icon(Icons.check_circle, color: AppColors.success, size: 18),
                const SizedBox(width: 6),
                Text(
                  '已是最新版本 ${_currentVersion ?? ''}',
                  style: const TextStyle(
                    fontSize: 14,
                    color: AppColors.success,
                    fontWeight: FontWeight.w500,
                  ),
                ),
              ],
            ),
          ),
        ],
      );
    }
    
    if (!hasUpdate && !p.otaConfirming) {
      return const SizedBox.shrink();
    }
    
    return Column(
      children: [
        if (hasUpdate) ...[
          Text(
            '${_currentVersion ?? ''} → ${_task?['target'] ?? ''}',
            style: const TextStyle(
              fontSize: 14,
              color: AppColors.textSecondary,
            ),
          ),
        ],
        if (p.otaConfirming || (status == 2 || status == 3)) ...[
          const SizedBox(height: 16),
          _buildProgressDetails(p),
        ],
      ],
    );
  }

  Widget _buildProgressDetails(ParkingProvider p) {
    final statusText = p.otaStatusText;
    final step = p.otaStep;
    final failed = p.otaStatus == 5 || p.otaStatus == 6;
    
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(
            statusText,
            style: const TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w500,
              color: AppColors.textPrimary,
            ),
          ),
          Text(
            '$step%',
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: failed ? AppColors.danger : AppColors.primary,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildActionButton(ParkingProvider p) {
    if (_loading) {
      return const SizedBox.shrink();
    }
    
    final status = p.otaStatus;
    final isConfirming = p.otaConfirming;
    final isSuccess = status == 4;
    final isFailed = status == 5 || status == 6;
    final hasUpdate = _task != null && status == 1;
    
    String buttonText;
    Color buttonColor;
    VoidCallback? onPressed;
    
    if (isConfirming) {
      buttonText = '升级中';
      buttonColor = AppColors.textSecondary;
      onPressed = null;
    } else if (isSuccess) {
      buttonText = '完成';
      buttonColor = AppColors.textSecondary;
      onPressed = () => Navigator.pop(context);
    } else if (isFailed) {
      buttonText = '重试';
      buttonColor = AppColors.danger;
      onPressed = _confirmUpgrade;
    } else if (hasUpdate) {
      buttonText = '立即更新';
      buttonColor = AppColors.primary;
      onPressed = _confirmUpgrade;
    } else {
      buttonText = '已是最新';
      buttonColor = AppColors.textSecondary;
      onPressed = null;
    }
    
    return SizedBox(
      width: double.infinity,
      height: 52,
      child: FilledButton(
        onPressed: onPressed,
        style: FilledButton.styleFrom(
          backgroundColor: buttonColor,
          disabledBackgroundColor: buttonColor.withValues(alpha: 0.5),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(26),
          ),
        ),
        child: Text(
          buttonText,
          style: const TextStyle(
            fontSize: 16,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
    );
  }
}
