import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../shared/widgets/page_header.dart';
import 'spot_detail_page.dart';

class SpotsPage extends StatefulWidget {
  const SpotsPage({super.key});

  @override
  State<SpotsPage> createState() => _SpotsPageState();
}

class _SpotsPageState extends State<SpotsPage> with TickerProviderStateMixin {
  /* ===== 模式切换: 弹簧挤压 =====
   * 点标题切换真实/模拟. 旧列表所有卡片向第一张堆叠挤压(弹簧压下),
   * 中点切换数据, 新列表从堆叠点弹簧弹开 → "归为一, 再散开"的弹簧效果.
   * 逐卡 Transform.translate: 第 i 张位移 -i*step*stackFactor, stackFactor=1 全叠到首张. */
  late final AnimationController _squeezeController = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 720),
  );
  // 单卡片行高估算(含间距), 用于逐卡堆叠位移. 列表布局实测 ~72.
  static const double _stackStep = 72;

  bool _switching = false;
  List<SpotModel>? _oldSpots;

  @override
  void dispose() {
    _squeezeController.dispose();
    super.dispose();
  }

  Future<void> _onToggleMode() async {
    if (_switching) return;
    _switching = true;
    final provider = context.read<ParkingProvider>();

    // 1. 快照旧列表(转场前半段固定不变)
    _oldSpots = List.of(provider.spots);
    if (mounted) setState(() {});

    try {
      // 2. ⭐ 同步切模式: 纯内存过滤缓存, 0 延迟. 不再等 OneNET API, 不再卡.
      provider.toggleRealModeSync();
      // 2.1 setState 让 AnimatedBuilder 闭包捕获新 spots (从 _realOnly 过滤后的新列表)
      if (mounted) setState(() {});
      // 3. 动画: 旧堆叠压缩(0~0.5) → 中点换数据 → 新弹簧弹开(0.5~1.0)
      await _squeezeController.forward(from: 0);
      // 4. 动画结束后 → 异步后台 refresh 同步云端最新数据 (不 await 不阻塞 UI, 失败有 3s 定时兜底)
      provider.finishModeSwitchAndRefresh();
    } finally {
      _oldSpots = null;
      _squeezeController.value = 0;
      if (mounted) setState(() {});
      _switching = false;
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final spots = provider.spots;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: provider.isLoading && spots.isEmpty
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              color: AppColors.primary,
              onRefresh: provider.refresh,
              child: SingleChildScrollView(
                physics: const AlwaysScrollableScrollPhysics(),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _buildHeader(context, provider),
                    const SizedBox(height: 16),
                    Padding(
                      padding: const EdgeInsets.fromLTRB(
                        AppDims.paddingPage,
                        0,
                        AppDims.paddingPage,
                        AppDims.paddingPage,
                      ),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          _buildSpotArea(context, provider, spots),
                          const SizedBox(height: 80),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
    );
  }

  /// 车位列表区: 模式切换时弹簧挤压转场, 平时直接渲染当前列表.
  Widget _buildSpotArea(
      BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    final current = _buildSpotLayout(context, provider, spots);
    // 非转场中 或 控制器空闲: 直接返回
    if (_oldSpots == null || (_squeezeController.isDismissed && !_switching)) {
      return current;
    }
    // 转场: AnimatedBuilder 每帧按进度重排卡片
    return AnimatedBuilder(
      animation: _squeezeController,
      builder: (context, _) {
        if (_oldSpots == null) return current; // 转场已结束, 直接返回新内容
        final tv = _squeezeController.value;
        // [0,0.5] 压缩旧快照, [0.5,1] 弹开新列表
        final squeeze = tv < 0.5 ? (tv / 0.5) : 1.0; // 0→1 旧压缩
        final expand = tv < 0.5 ? 0.0 : ((tv - 0.5) / 0.5); // 0→1 新弹开
        // 压缩用 easeInCubic(加速压下), 弹开用 easeOutBack(过冲回弹, 弹簧感)
        final sqC = Curves.easeInCubic.transform(squeeze);
        final exC = Curves.easeOutBack.transform(expand);
        final showOld = tv < 0.5;
        final list = showOld ? _oldSpots! : spots;
        // stackFactor: 1=完全堆叠到第一张, 0=完全展开. 旧阶段 0→1, 新阶段 1→0
        final stackFactor = (showOld ? sqC : (1.0 - exC)).clamp(0.0, 1.0);
        return _buildStackedLayout(context, provider, list, stackFactor);
      },
    );
  }

  /// 堆叠布局: 按 layoutMode 渲染, 每张卡片向第一张(顶部/左上)堆叠.
  Widget _buildStackedLayout(BuildContext context, ParkingProvider provider,
      List<SpotModel> spots, double stackFactor) {
    if (spots.isEmpty) return _buildSpotLayout(context, provider, spots);
    switch (provider.layoutMode) {
      case 1:
        return _buildStackedGrid(context, provider, spots, stackFactor);
      case 0:
      default:
        return _buildStackedList(context, provider, spots, stackFactor);
    }
  }

  /// 列表布局逐卡堆叠: 第 i 张向上移 i*step*stackFactor, stackFactor=1 全叠到首张.
  Widget _buildStackedList(BuildContext context, ParkingProvider provider,
      List<SpotModel> spots, double stackFactor) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: List.generate(spots.length, (i) {
        final item = _buildSpotListItem(context, provider, spots[i]);
        return Transform.translate(
          offset: Offset(0, -i * _stackStep * stackFactor),
          child: Opacity(
            opacity: (1 - stackFactor * 0.6).clamp(0.0, 1.0),
            child: item,
          ),
        );
      }),
    );
  }

  /// 网格布局逐卡堆叠: 第 (row,col) 张向 (0,0) 位置移动.
  Widget _buildStackedGrid(BuildContext context, ParkingProvider provider,
      List<SpotModel> spots, double stackFactor) {
    return LayoutBuilder(builder: (context, c) {
      const crossSpacing = 10.0, mainSpacing = 10.0;
      final cellW = (c.maxWidth - crossSpacing) / 2;
      final cellH = cellW / 2.0; // aspectRatio 2.0
      final colStep = cellW + crossSpacing;
      final rowStep = cellH + mainSpacing;
      return Wrap(
        spacing: crossSpacing,
        runSpacing: mainSpacing,
        children: List.generate(spots.length, (i) {
          final row = i ~/ 2, col = i % 2;
          return SizedBox(
            width: cellW,
            height: cellH,
            child: Transform.translate(
              offset: Offset(
                -col * colStep * stackFactor,
                -row * rowStep * stackFactor,
              ),
              child: Opacity(
                opacity: (1 - stackFactor * 0.6).clamp(0.0, 1.0),
                child: _buildSpotGridItem(context, provider, spots[i]),
              ),
            ),
          );
        }),
      );
    });
  }

  Widget _buildHeader(BuildContext context, ParkingProvider provider) {
    final search = Container(
      width: 40,
      height: 40,
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(12),
      ),
      child: const Icon(Icons.search, color: AppColors.textSecondary, size: 22),
    );
    // 隐藏式触发: 点"车位"标题切换真实/模拟, 配合交叉溶解转场
    return PageHeader(
      title: '车位',
      onTitleTap: _onToggleMode,
      actions: [_buildLayoutSwitcher(provider), const SizedBox(width: 10), search],
    );
  }

  Widget _buildLayoutSwitcher(ParkingProvider provider) {
    final mode = provider.layoutMode;
    return GestureDetector(
      onTap: () => provider.setLayoutMode(mode == 0 ? 1 : 0),
      behavior: HitTestBehavior.opaque,
      child: Container(
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: AppColors.surface,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Icon(_layoutIcon(mode), color: AppColors.textSecondary, size: 22),
      ),
    );
  }

  IconData _layoutIcon(int mode) {
    return mode == 0 ? Icons.view_list : Icons.grid_view;
  }

  Widget _buildSpotLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    if (spots.isEmpty) {
      return Container(
        padding: const EdgeInsets.symmetric(vertical: 60),
        alignment: Alignment.center,
        child: const Column(
          children: [
            Icon(Icons.local_parking, size: 48, color: AppColors.textSecondary),
            SizedBox(height: 12),
            Text(
              '暂无车位',
              style: TextStyle(fontSize: 14, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    switch (provider.layoutMode) {
      case 1:
        return _buildGridLayout(context, provider, spots);
      case 0:
      default:
        return _buildListLayout(context, provider, spots);
    }
  }

  /* ==================== 列表布局 ==================== */

  Widget _buildListLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    return ListView.builder(
      shrinkWrap: true,
      padding: EdgeInsets.zero, // ⭐ 去掉 MediaQuery 自动加的顶部 padding, 让标题到首卡距离和堆叠动画一致
      physics: const NeverScrollableScrollPhysics(),
      itemCount: spots.length,
      itemBuilder: (context, index) {
        return _buildSpotListItem(context, provider, spots[index]);
      },
    );
  }

  Widget _buildSpotListItem(BuildContext context, ParkingProvider provider, SpotModel spot) {
    final bgColor = _getSpotColor(spot);
    final statusText = _getSpotStatusText(spot);
    final iconData = _getSpotIcon(spot);
    final canAct = !spot.isFree && !spot.isOffline && !spot.isDisabledSpot;

    return GestureDetector(
      onTap: () => _navigateToDetail(context, spot),
      child: Container(
        margin: const EdgeInsets.only(bottom: 8),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
          child: Row(
            children: [
              GestureDetector(
                onTap: () {
                  // 模拟车位: 点击图标循环切换状态(空闲→占用→僵尸), 真实设备照常进详情
                  if (!spot.isReal) {
                    provider.cycleMockStatus(spot.id);
                  } else {
                    _navigateToDetail(context, spot);
                  }
                },
                child: Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    color: bgColor.withValues(alpha: 0.12),
                    borderRadius: BorderRadius.circular(10),
                  ),
                  child: Icon(iconData, color: bgColor, size: 22),
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Text(
                          spot.id,
                          style: const TextStyle(
                            fontSize: 15,
                            fontWeight: FontWeight.w700,
                            color: AppColors.textPrimary,
                          ),
                        ),
                        const SizedBox(width: 8),
                        Container(
                          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                          decoration: BoxDecoration(
                            color: bgColor.withValues(alpha: 0.12),
                            borderRadius: BorderRadius.circular(6),
                          ),
                          child: Text(
                            statusText,
                            style: TextStyle(
                              fontSize: 11,
                              fontWeight: FontWeight.w600,
                              color: bgColor,
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 4),
                    Row(
                      children: [
                        Expanded(
                          child: Text(
                            _getSpotSubText(spot),
                            style: const TextStyle(
                              fontSize: 12,
                              color: AppColors.textSecondary,
                            ),
                          ),
                        ),
                        if (canAct) ...[
                          if (spot.isNotified)
                            const Icon(Icons.notifications_active, size: 14, color: AppColors.primary),
                          if (spot.isNotified && provider.isSpotDispatched(spot.id))
                            const SizedBox(width: 4),
                          if (provider.isSpotDispatched(spot.id))
                            const Icon(Icons.assignment, size: 14, color: AppColors.warning),
                        ],
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  /* ==================== 网格布局 ==================== */

  Widget _buildGridLayout(BuildContext context, ParkingProvider provider, List<SpotModel> spots) {
    return GridView.builder(
      shrinkWrap: true,
      padding: EdgeInsets.zero, // ⭐ 去掉 MediaQuery 自动加的顶部 padding
      physics: const NeverScrollableScrollPhysics(),
      gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 2,
        crossAxisSpacing: 10,
        mainAxisSpacing: 10,
        childAspectRatio: 2.0,
      ),
      itemCount: spots.length,
      itemBuilder: (context, index) {
        return _buildSpotGridItem(context, provider, spots[index]);
      },
    );
  }

  Widget _buildSpotGridItem(BuildContext context, ParkingProvider provider, SpotModel spot) {
    final bgColor = _getSpotColor(spot);
    final statusText = _getSpotStatusText(spot);

    return GestureDetector(
      onTap: () => _navigateToDetail(context, spot),
      child: Container(
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(12),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.08),
              blurRadius: 8,
              offset: const Offset(0, 2),
            ),
          ],
        ),
        child: Padding(
          padding: const EdgeInsets.all(10),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      spot.id,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 14,
                        fontWeight: FontWeight.w700,
                        color: AppColors.textPrimary,
                      ),
                    ),
                  ),
                  const SizedBox(width: 6),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                    decoration: BoxDecoration(
                      color: bgColor.withValues(alpha: 0.12),
                      borderRadius: BorderRadius.circular(4),
                    ),
                    child: Text(
                      statusText,
                      style: TextStyle(
                        fontSize: 9,
                        fontWeight: FontWeight.w600,
                        color: bgColor,
                      ),
                    ),
                  ),
                ],
              ),
              const Spacer(),
              Text(
                _getSpotSubText(spot),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  fontSize: 11,
                  color: AppColors.textSecondary,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  /* ==================== 辅助方法 ==================== */

  Color _getSpotColor(SpotModel spot) {
    if (spot.isDisabledSpot) return AppColors.textSecondary;
    if (spot.isOffline) return AppColors.textSecondary;
    if (spot.isFree) return AppColors.success;
    if (spot.isOccupied) return AppColors.warning;
    return AppColors.danger;
  }

  String _getSpotStatusText(SpotModel spot) {
    if (spot.isDisabledSpot) return '已停用';
    if (spot.isOffline) return '离线';
    if (spot.isFree) return '空闲';
    if (spot.isOccupied) return '占用';
    return '僵尸车';
  }

  IconData _getSpotIcon(SpotModel spot) {
    if (spot.isDisabledSpot) return Icons.block;
    if (spot.isOffline) return Icons.cloud_off;
    if (spot.isFree) return Icons.local_parking;
    if (spot.isOccupied) return Icons.directions_car;
    return Icons.warning_amber;
  }

  String _getSpotSubText(SpotModel spot) {
    if (spot.isDisabledSpot) return '设备已停用';
    if (spot.isOffline) return '设备离线';
    if (spot.isFree) return '暂无车辆';
    return '占用 ${SpotModel.formatOccupiedDuration(spot.actualOccupiedSec)}';
  }

  void _navigateToDetail(BuildContext context, SpotModel spot) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => SpotDetailPage(spot: spot)),
    );
  }

  void _showSpotMenu(BuildContext context, SpotModel spot) {
    showModalBottomSheet(
      context: context,
      backgroundColor: Colors.transparent,
      isScrollControlled: true,
      builder: (context) {
        return Container(
          margin: const EdgeInsets.fromLTRB(16, 0, 16, 16),
          decoration: BoxDecoration(
            color: Colors.white,
            borderRadius: BorderRadius.circular(16),
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 20, 20, 8),
                child: Row(
                  children: [
                    Text(
                      spot.id,
                      style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                    ),
                    const Spacer(),
                    GestureDetector(
                      onTap: () => Navigator.pop(context),
                      child: const Icon(Icons.close, size: 22, color: AppColors.textSecondary),
                    ),
                  ],
                ),
              ),
              const Divider(height: 1),
              ListTile(
                leading: const Icon(Icons.info_outline, color: AppColors.primary, size: 22),
                title: const Text(
                  '查看详情',
                  style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                ),
                subtitle: const Text('查看车位、车辆、设备与告警信息', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                onTap: () {
                  Navigator.pop(context);
                  Navigator.push(
                    context,
                    MaterialPageRoute(builder: (_) => SpotDetailPage(spot: spot)),
                  );
                },
              ),
              const SizedBox(height: 8),
            ],
          ),
        );
      },
    );
  }
}
