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
    // ⭐ 检查中最小展示 1.5s, 避免 API 太快返回导致"检查中"一闪而过让人反应不过来.
    // 计时与 API 请求并行, 仅在 API 快于 1.5s 时才等待补足, 不拖慢实际慢的情况
    final minDisplay = Future<void>.delayed(const Duration(milliseconds: 1500));
    final version = await _provider.getOtaNodeVersion(_deviceName) ?? _defaultVersion;
    final task = await _provider.getOtaTask(_deviceName, version: version);
    await minDisplay;
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
    if (status == 5 || status == 6) return '确定';
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
              // 核心区域: 圆环/logo/公告卡片 + 版本信息, 黄金分割定位 (内容中心在内容区距顶 38.2% 处)
              // ⭐ AnimatedSwitcher: 状态切换(发现新版本卡片 ↔ 升级圆环)时淡入淡出+缩放, 丝滑无割裂
              Expanded(
                child: Align(
                  alignment: const Alignment(0, -0.236),
                  child: AnimatedSwitcher(
                    // ⭐ 丝滑过渡: 进入用 easeOutCubic(先快后慢, 像物体自然落定),
                    // 退出用 easeInCubic(先慢后快, 像物体自然飞出), 非对称曲线
                    // 贴合物理直觉, 比匀速线性自然得多; 叠加轻微 Y 位移 + 微缩放,
                    // 给"流动感"避免单纯淡入淡出的呆板
                    duration: const Duration(milliseconds: 420),
                    switchInCurve: Curves.easeOutCubic,
                    switchOutCurve: Curves.easeInCubic,
                    transitionBuilder: (child, anim) {
                      return FadeTransition(
                        opacity: anim,
                        child: SlideTransition(
                          position: Tween<Offset>(
                            begin: const Offset(0, 0.05),
                            end: Offset.zero,
                          ).animate(anim),
                          child: ScaleTransition(
                            scale: Tween<double>(begin: 0.96, end: 1.0)
                                .animate(anim),
                            child: child,
                          ),
                        ),
                      );
                    },
                    child: Column(
                      key: ValueKey(_stateKey(p)),
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

    // ⭐ 发现新版本: 用公告卡片替代空圆环(进度恒0占位无意义), 信息密度更高
    if (hasUpdate) {
      return _buildUpdateCard();
    }
    // 检查中 / 升级中 / 已完成 / 失败 → 显示环形进度条
    if (_loading || isUpdating || isDoneOrFailed) {
      return _buildProgressRing(ringProgress, ringColor, p);
    }
    // 无新版本 → 显示 logo
    return _buildNoUpdateLogo();
  }

  /// 状态键: 决定 AnimatedSwitcher 是否触发过渡动画.
  /// 状态切换(发现新版本卡片 ↔ 升级圆环 ↔ 完成圆环...)时 key 变化 → 淡入淡出+缩放丝滑切换.
  String _stateKey(ParkingProvider p) {
    if (_loading) return 'loading';
    final status = p.otaStatus;
    if (_task != null && status == 1) return 'update';
    if (p.otaConfirming || status == 2 || status == 3) return 'updating';
    if (status == 4) return 'done';
    if (status == 5 || status == 6) return 'failed';
    return 'noupdate';
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

  /// ⭐ 发现新版本公告卡片 (取代发现阶段的空圆环).
  /// 进度圆环在"未开始升级"时进度恒为0, 占着大位置却无信息量, 显得空荡.
  /// 改用信息密度高的卡片: 标签 + 新版本号 + 旧→新胶囊 + 升级要点.
  Widget _buildUpdateCard() {
    final target = _task?['target']?.toString() ?? '';
    final current = _currentVersion ?? '';
    return Container(
      width: 300,
      padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 28),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(20),
        boxShadow: [
          BoxShadow(
            color: AppColors.primary.withValues(alpha: 0.08),
            blurRadius: 30,
            offset: const Offset(0, 12),
          ),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // 顶部"发现新版本"标签
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
            decoration: BoxDecoration(
              color: AppColors.primary.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.system_update, size: 14, color: AppColors.primary),
                const SizedBox(width: 4),
                const Text(
                  '发现新版本',
                  style: TextStyle(
                    fontSize: 12,
                    color: AppColors.primary,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 20),
          // 新版本号大字
          Text(
            target,
            style: const TextStyle(
              fontSize: 40,
              fontWeight: FontWeight.w600,
              color: AppColors.primary,
              height: 1.1,
            ),
          ),
          const SizedBox(height: 16),
          // 旧→新 胶囊对比
          Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              _buildVersionChip(current, isOld: true),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 10),
                child: Icon(Icons.arrow_forward, size: 16, color: AppColors.textSecondary),
              ),
              _buildVersionChip(target, isOld: false),
            ],
          ),
          const SizedBox(height: 20),
          // 分隔线
          Container(
            height: 0.5,
            color: AppColors.textSecondary.withValues(alpha: 0.15),
          ),
          const SizedBox(height: 16),
          // 升级要点
          _buildBulletPoint('修复已知问题'),
          _buildBulletPoint('提升系统稳定性'),
          _buildBulletPoint('优化运行性能'),
        ],
      ),
    );
  }

  Widget _buildVersionChip(String ver, {required bool isOld}) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
      decoration: BoxDecoration(
        color: isOld
            ? AppColors.textSecondary.withValues(alpha: 0.12)
            : AppColors.primary.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Text(
        ver,
        style: TextStyle(
          fontSize: 13,
          fontWeight: FontWeight.w600,
          color: isOld ? AppColors.textSecondary : AppColors.primary,
        ),
      ),
    );
  }

  Widget _buildBulletPoint(String text) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          Icon(Icons.check_circle, size: 16, color: AppColors.primary.withValues(alpha: 0.7)),
          const SizedBox(width: 8),
          Text(text, style: const TextStyle(fontSize: 13, color: AppColors.textSecondary)),
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
    
    // ⭐ 发现新版本阶段: 公告卡片已含旧→新对比, 这里不重复显示
    if (hasUpdate) {
      return const SizedBox.shrink();
    }
    if (!p.otaConfirming) {
      return const SizedBox.shrink();
    }

    // ⭐ 升级中: 圆环已显示百分比+状态文字, 不再重复显示进度卡片
    // 仅保留升级60s未启动提示(不结束升级, 仅提示用户检查设备)
    if (p.otaStallHint) {
      return Padding(
        padding: const EdgeInsets.only(top: 12),
        child: _buildStallHint(),
      );
    }
    return const SizedBox.shrink();
  }

  /// ⭐ 升级60s未启动提示(不结束升级, 仅提示用户检查设备)
  Widget _buildStallHint() {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: Colors.orange.withValues(alpha: 0.1),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: Colors.orange.withValues(alpha: 0.4), width: 1),
      ),
      child: const Row(
        children: [
          Icon(Icons.info_outline, size: 16, color: Colors.orange),
          SizedBox(width: 8),
          Expanded(
            child: Text(
              '升级似乎未启动，请检查节点是否在线或网关是否正常分发',
              style: TextStyle(fontSize: 12, color: Colors.orange),
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
      buttonText = '确定';
      buttonColor = AppColors.danger;
      onPressed = () => Navigator.pop(context);
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
