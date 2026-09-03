import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/gateway_model.dart';
import '../../../../core/providers/parking_provider.dart';
import 'gateway_detail_page.dart';

/// 设备中心 (多网关): 顶部全局汇总 + 网关列表, 点网关进节点详情.
///
/// 真实模式: 网关与子设备均来自 OneNET (device/list + childdevice/list).
/// 模拟模式: 用本地车位列表派生 3 个模拟网关, 演示多网关架构格局.
class DeviceCenterPage extends StatefulWidget {
  const DeviceCenterPage({super.key});

  @override
  State<DeviceCenterPage> createState() => _DeviceCenterPageState();
}

class _DeviceCenterPageState extends State<DeviceCenterPage> {
  List<GatewayModel> _gateways = const [];
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    // 等首帧 build 完再读 provider, 避免 build 期间触发 setState
    WidgetsBinding.instance.addPostFrameCallback((_) => _load());
  }

  Future<void> _load() async {
    final provider = context.read<ParkingProvider>();
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final gateways = provider.realOnly
          ? await _loadReal(provider)
          : _loadSimulated(provider);
      setState(() {
        _gateways = gateways;
        _loading = false;
      });
    } catch (e) {
      setState(() {
        _error = '加载失败: $e';
        _loading = false;
      });
    }
  }

  /// 真实模式: 拉所有网关, 并行拉各自子设备
  Future<List<GatewayModel>> _loadReal(ParkingProvider provider) async {
    final raw = await provider.getGateways();
    if (raw.isEmpty) return const [];
    final models = raw.map(GatewayModel.fromMap).toList();
    // 并行拉每个网关的子设备, 加快首屏
    final childrenFutures = models.map((g) => provider.getGatewayChildren(g.name)).toList();
    final childrenLists = await Future.wait(childrenFutures);
    return [
      for (var i = 0; i < models.length; i++)
        GatewayModel(
          name: models[i].name,
          online: models[i].online,
          isDisabled: models[i].isDisabled,
          isReal: true,
          children: childrenLists[i],
        ),
    ];
  }

  /// 模拟模式: 用本地车位列表派生 3 个模拟网关 (格局展示用, 不调平台)
  List<GatewayModel> _loadSimulated(ParkingProvider provider) {
    return GatewayModel.buildSimulated(provider.spots, count: 3);
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    return Scaffold(
      backgroundColor: AppColors.background,
      body: SafeArea(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _buildAppBar(context),
            Expanded(
              child: RefreshIndicator(
                color: AppColors.primary,
                onRefresh: _load,
                child: _loading
                    ? const Center(child: CircularProgressIndicator())
                    : (_gateways.isEmpty
                        ? _buildEmpty(provider)
                        : _buildList()),
              ),
            ),
          ],
        ),
      ),
    );
  }

  /* ==================== 顶部栏 ==================== */
  Widget _buildAppBar(BuildContext context) {
    final realOnly = context.select<ParkingProvider, bool>((p) => p.realOnly);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Row(
        children: [
          IconButton(
            icon: const Icon(Icons.arrow_back, color: AppColors.textPrimary),
            onPressed: () => Navigator.of(context).pop(),
          ),
          const Text(
            '设备中心',
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
          ),
          const Spacer(),
          // 模式标记
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            decoration: BoxDecoration(
              color: AppColors.primary.withValues(alpha: 0.08),
              borderRadius: BorderRadius.circular(AppDims.radiusSmall),
            ),
            child: Text(
              realOnly ? '真实模式' : '模拟模式',
              style: const TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: AppColors.primary),
            ),
          ),
        ],
      ),
    );
  }

  /* ==================== 汇总卡 ==================== */
  Widget _buildSummary() {
    final total = _gateways.length;
    final online = _gateways.where((g) => g.online && !g.isDisabled).length;
    final totalNodes = _gateways.fold<int>(0, (s, g) => s + g.children.length);
    final onlineNodes = _gateways.fold<int>(0, (s, g) => s + g.onlineChildren);
    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Row(
            children: [
              Icon(Icons.hub, size: 18, color: AppColors.primary),
              SizedBox(width: 8),
              Text('全网汇总', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            ],
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              _buildStat('网关', '$online', '/ $total 在线', AppColors.primary),
              _buildStat('节点', '$onlineNodes', '/ $totalNodes 在线', AppColors.success),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildStat(String label, String main, String suffix, Color color) {
    return Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
          const SizedBox(height: 4),
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              Text(main, style: TextStyle(fontSize: 22, fontWeight: FontWeight.w700, color: color)),
              const SizedBox(width: 4),
              Text(suffix, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
            ],
          ),
        ],
      ),
    );
  }

  /* ==================== 网关列表 ==================== */
  Widget _buildList() {
    return SingleChildScrollView(
      physics: const AlwaysScrollableScrollPhysics(),
      padding: const EdgeInsets.fromLTRB(AppDims.paddingPage, 4, AppDims.paddingPage, AppDims.paddingPage),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _buildSummary(),
          const SizedBox(height: 16),
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 4),
            child: Text(
              '网关列表 (${_gateways.length})',
              style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
            ),
          ),
          const SizedBox(height: 8),
          ..._gateways.map((g) => Padding(
            padding: const EdgeInsets.only(bottom: 10),
            child: _buildGatewayCard(context, g),
          )),
          const SizedBox(height: 32),
        ],
      ),
    );
  }

  Widget _buildGatewayCard(BuildContext context, GatewayModel g) {
    final total = g.children.length;
    final online = g.onlineChildren;
    final rate = total > 0 ? online / total : 0.0;
    final onlinePct = (rate * 100).round();
    final badgeColor = g.isDisabled
        ? AppColors.textSecondary
        : (g.online ? AppColors.success : AppColors.danger);
    final statusText = g.isDisabled ? '已停用' : (g.online ? '在线' : '离线');
    return CardContainer(
      onTap: () => _pushDetail(context, g),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(Icons.router, size: 20, color: badgeColor),
              const SizedBox(width: 8),
              Text(g.name, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
              if (!g.isReal) ...[
                const SizedBox(width: 8),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  decoration: BoxDecoration(
                    color: AppColors.primary.withValues(alpha: 0.1),
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: const Text('模拟', style: TextStyle(fontSize: 10, fontWeight: FontWeight.w600, color: AppColors.primary)),
                ),
              ],
              const Spacer(),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                decoration: BoxDecoration(
                  color: badgeColor.withValues(alpha: 0.1),
                  borderRadius: BorderRadius.circular(20),
                  border: Border.all(color: badgeColor.withValues(alpha: 0.3), width: 1),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Container(width: 8, height: 8, decoration: BoxDecoration(color: badgeColor, shape: BoxShape.circle)),
                    const SizedBox(width: 6),
                    Text(statusText, style: TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: badgeColor)),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 14),
          Row(
            children: [
              Text('节点在线 $online/$total', style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
              const SizedBox(width: 8),
              Expanded(
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(3),
                  child: LinearProgressIndicator(
                    value: rate,
                    backgroundColor: AppColors.textSecondary.withValues(alpha: 0.1),
                    valueColor: AlwaysStoppedAnimation(online > 0 ? AppColors.success : Colors.grey),
                    minHeight: 6,
                  ),
                ),
              ),
              const SizedBox(width: 8),
              Text('$onlinePct%', style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600, color: AppColors.textSecondary)),
            ],
          ),
          const SizedBox(height: 10),
          const Row(
            children: [
              Icon(Icons.chevron_right, size: 16, color: AppColors.textSecondary),
              SizedBox(width: 2),
              Text('查看下挂节点', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
            ],
          ),
        ],
      ),
    );
  }

  /* ==================== 空/错误态 ==================== */
  Widget _buildEmpty(ParkingProvider provider) {
    final msg = _error ?? '暂无网关设备';
    final tip = provider.realOnly ? '请在 OneNET 网关产品下添加网关设备' : '请先在车位列表中添加模拟车位';
    return SingleChildScrollView(
      physics: const AlwaysScrollableScrollPhysics(),
      padding: const EdgeInsets.all(AppDims.paddingPage),
      child: Column(
        children: [
          const SizedBox(height: 80),
          Icon(Icons.router_outlined, size: 56, color: AppColors.textSecondary.withValues(alpha: 0.4)),
          const SizedBox(height: 12),
          Text(msg, style: const TextStyle(fontSize: 14, color: AppColors.textSecondary)),
          const SizedBox(height: 4),
          Text(tip, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
        ],
      ),
    );
  }

  /* ==================== 跳转 ==================== */
  void _pushDetail(BuildContext context, GatewayModel g) {
    Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => GatewayDetailPage(gateway: g)),
    );
  }
}
