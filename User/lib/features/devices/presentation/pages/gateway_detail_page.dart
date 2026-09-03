import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/models/gateway_model.dart';
import '../../../../core/providers/parking_provider.dart';

/// 网关详情页: 网关状态卡 + 下挂节点列表.
///
/// 真实网关: 刷新走 OneNET childdevice/list.
/// 模拟网关: 刷新从本地车位列表重新派生 (不调平台).
class GatewayDetailPage extends StatefulWidget {
  final GatewayModel gateway;

  const GatewayDetailPage({super.key, required this.gateway});

  @override
  State<GatewayDetailPage> createState() => _GatewayDetailPageState();
}

class _GatewayDetailPageState extends State<GatewayDetailPage> {
  late GatewayModel _gateway;
  bool _loading = false;

  @override
  void initState() {
    super.initState();
    _gateway = widget.gateway;
  }

  Future<void> _refresh() async {
    if (_gateway.isReal) {
      setState(() => _loading = true);
      try {
        final provider = context.read<ParkingProvider>();
        final children = await provider.getGatewayChildren(_gateway.name);
        setState(() {
          _gateway = GatewayModel(
            name: _gateway.name,
            online: _gateway.online,
            isDisabled: _gateway.isDisabled,
            isReal: true,
            children: children,
          );
          _loading = false;
        });
      } catch (_) {
        setState(() => _loading = false);
      }
    } else {
      // 模拟网关: 从本地车位列表重新派生, 取同名网关的最新子设备
      final provider = context.read<ParkingProvider>();
      final latest = GatewayModel.buildSimulated(provider.spots, count: 3);
      final match = latest.firstWhere(
        (g) => g.name == _gateway.name,
        orElse: () => _gateway,
      );
      setState(() => _gateway = match);
    }
  }

  @override
  Widget build(BuildContext context) {
    final spots = _gateway.children;
    final total = spots.length;
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
                onRefresh: _refresh,
                child: SingleChildScrollView(
                  physics: const AlwaysScrollableScrollPhysics(),
                  padding: const EdgeInsets.fromLTRB(AppDims.paddingPage, 4, AppDims.paddingPage, AppDims.paddingPage),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _buildGatewayCard(),
                      const SizedBox(height: 16),
                      Padding(
                        padding: const EdgeInsets.symmetric(vertical: 4),
                        child: Text(
                          '下挂节点 ($total)',
                          style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                        ),
                      ),
                      const SizedBox(height: 8),
                      if (spots.isEmpty)
                        _buildEmptyNodes()
                      else
                        ...spots.map((s) => Padding(
                          padding: const EdgeInsets.only(bottom: 10),
                          child: _buildNodeCard(s),
                        )),
                      const SizedBox(height: 32),
                    ],
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  /* ==================== 顶部栏 ==================== */
  Widget _buildAppBar(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Row(
        children: [
          IconButton(
            icon: const Icon(Icons.arrow_back, color: AppColors.textPrimary),
            onPressed: () => Navigator.of(context).pop(),
          ),
          Text(
            _gateway.name,
            style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
          ),
          if (!_gateway.isReal) ...[
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
          IconButton(
            icon: _loading
                ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2, color: AppColors.primary))
                : const Icon(Icons.refresh, color: AppColors.textPrimary),
            onPressed: _loading ? null : _refresh,
          ),
        ],
      ),
    );
  }

  /* ==================== 网关状态卡 ==================== */
  Widget _buildGatewayCard() {
    final total = _gateway.children.length;
    final online = _gateway.onlineChildren;
    final rate = total > 0 ? online / total : 0.0;
    final onlinePct = (rate * 100).round();
    final badgeColor = _gateway.isDisabled
        ? AppColors.textSecondary
        : (_gateway.online ? AppColors.success : AppColors.danger);
    final statusText = _gateway.isDisabled ? '已停用' : (_gateway.online ? '在线' : '离线');
    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(Icons.router, size: 20, color: badgeColor),
              const SizedBox(width: 8),
              Text(_gateway.name, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
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
          const SizedBox(height: 16),
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
        ],
      ),
    );
  }

  /* ==================== 节点卡 ==================== */
  Widget _buildNodeCard(SpotModel s) {
    final statusColor = s.isDisabledSpot
        ? AppColors.warning
        : (s.isOnline ? AppColors.success : Colors.grey);
    final statusText = s.isDisabledSpot ? '已停用' : (s.isOnline ? '在线' : '离线');
    final spotStatusText = _spotStatusLabel(s.status);
    return CardContainer(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Text(s.id, style: const TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
              if (!s.isReal) ...[
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
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                decoration: BoxDecoration(
                  color: statusColor.withValues(alpha: 0.1),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Container(width: 6, height: 6, decoration: BoxDecoration(color: statusColor, shape: BoxShape.circle)),
                    const SizedBox(width: 4),
                    Text(statusText, style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: statusColor)),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              _buildNodeDetail('车位', spotStatusText),
              _buildNodeDetail('电量', '${s.batteryLevel.toStringAsFixed(0)}%'),
              _buildNodeDetail('信号', s.isOnline ? '${s.signalStrength}dBm' : '-'),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildNodeDetail(String label, String value) {
    return Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(fontSize: 11, color: AppColors.textSecondary)),
          const SizedBox(height: 2),
          Text(value, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
        ],
      ),
    );
  }

  Widget _buildEmptyNodes() {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 40),
      child: Center(
        child: Column(
          children: [
            Icon(Icons.sensors_off, size: 40, color: AppColors.textSecondary.withValues(alpha: 0.4)),
            const SizedBox(height: 8),
            const Text('该网关暂无下挂节点', style: TextStyle(fontSize: 13, color: AppColors.textSecondary)),
          ],
        ),
      ),
    );
  }

  String _spotStatusLabel(String status) {
    switch (status) {
      case 'free': return '空闲';
      case 'occupied': return '占用';
      case 'zombie': return '僵尸车';
      case 'offline': return '离线';
      case 'disabled': return '停用';
      default: return status;
    }
  }
}
