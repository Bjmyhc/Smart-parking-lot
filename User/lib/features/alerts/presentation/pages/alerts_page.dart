import 'package:flutter/material.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/custom_app_bar.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/services/api_service.dart';

class AlertsPage extends StatefulWidget {
  const AlertsPage({super.key});

  @override
  State<AlertsPage> createState() => _AlertsPageState();
}

class _AlertsPageState extends State<AlertsPage> {
  final ApiService _apiService = ApiService();
  List<AlertModel> _alerts = [];
  bool _isLoading = true;
  bool _isMapView = false;

  @override
  void initState() {
    super.initState();
    _loadAlerts();
  }

  Future<void> _loadAlerts() async {
    setState(() {
      _isLoading = true;
    });
    try {
      final alerts = await _apiService.getAlerts();
      alerts.sort((a, b) => b.occupiedHours.compareTo(a.occupiedHours));
      if (mounted) {
        setState(() {
          _alerts = alerts;
          _isLoading = false;
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _alerts = [];
          _isLoading = false;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: const CustomAppBar(title: '僵尸车工单', showBackButton: true),
      body: _isLoading
          ? const Center(child: CircularProgressIndicator())
          : Column(
              children: [
                _buildViewSwitch(),
                Expanded(
                  child: _isMapView ? _buildMapView() : _buildListView(),
                ),
              ],
            ),
    );
  }

  Widget _buildViewSwitch() {
    return Container(
      padding: const EdgeInsets.all(AppDims.paddingPage),
      child: Container(
        padding: const EdgeInsets.all(4),
        decoration: BoxDecoration(
          color: AppColors.surface,
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: Row(
          children: [
            _buildTab('列表视图', false),
            _buildTab('车位地图', true),
          ],
        ),
      ),
    );
  }

  Widget _buildTab(String label, bool isMap) {
    final isSelected = _isMapView == isMap;
    return Expanded(
      child: GestureDetector(
        onTap: () {
          setState(() {
            _isMapView = isMap;
          });
        },
        child: Container(
          padding: const EdgeInsets.symmetric(vertical: 10),
          decoration: BoxDecoration(
            color: isSelected ? AppColors.primary : Colors.transparent,
            borderRadius: BorderRadius.circular(AppDims.radiusSmall),
          ),
          child: Text(
            label,
            textAlign: TextAlign.center,
            style: TextStyle(
              fontSize: 14,
              fontWeight: FontWeight.w600,
              color: isSelected ? AppColors.surface : AppColors.textSecondary,
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildListView() {
    if (_alerts.isEmpty) {
      return Center(
        child: Column(
          children: [
            const Icon(Icons.warning_amber, size: 64, color: AppColors.textSecondary),
            const SizedBox(height: 16),
            Text(
              '暂无僵尸车告警',
              style: const TextStyle(fontSize: 16, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    return RefreshIndicator(
      color: AppColors.primary,
      onRefresh: _loadAlerts,
      child: ListView.builder(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        itemCount: _alerts.length,
        itemBuilder: (context, index) {
          return _buildAlertItem(_alerts[index]);
        },
      ),
    );
  }

  Widget _buildAlertItem(AlertModel alert) {
    return _SlidableAlertItem(
      alert: alert,
      onDispatch: () async {
        await _apiService.dispatchAlert(alert.id);
        _showSnackBar('已派单');
      },
      onNotify: () async {
        await _apiService.notifyOwner(alert.id);
        _showSnackBar('已通知车主');
      },
      onResolve: () async {
        await _apiService.resolveAlert(alert.id);
        _showSnackBar('已处置');
        _loadAlerts();
      },
    );
  }

  Widget _buildMapView() {
    if (_alerts.isEmpty) {
      return Center(
        child: Column(
          children: [
            const Icon(Icons.map, size: 64, color: AppColors.textSecondary),
            const SizedBox(height: 16),
            Text(
              '暂无僵尸车标记',
              style: const TextStyle(fontSize: 16, color: AppColors.textSecondary),
            ),
          ],
        ),
      );
    }

    return Container(
      margin: const EdgeInsets.all(AppDims.paddingPage),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(AppDims.radiusLarge),
        boxShadow: [
          BoxShadow(
            color: AppColors.textPrimary.withOpacity(0.08),
            blurRadius: 12,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              border: Border(
                bottom: BorderSide(color: AppColors.textSecondary.withOpacity(0.2)),
              ),
            ),
            child: Row(
              children: [
                const Icon(Icons.map, color: AppColors.primary, size: 20),
                const SizedBox(width: 8),
                const Text(
                  '僵尸车位置分布',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
                ),
                const Spacer(),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  decoration: BoxDecoration(
                    color: AppColors.danger.withOpacity(0.1),
                    borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                  ),
                  child: Text(
                    '${_alerts.length} 辆',
                    style: const TextStyle(
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                      color: AppColors.danger,
                    ),
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            child: GridView.builder(
              padding: const EdgeInsets.all(16),
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 4,
                crossAxisSpacing: 12,
                mainAxisSpacing: 12,
              ),
              itemCount: 24,
              itemBuilder: (context, index) {
                AlertModel? alert;
                for (final a in _alerts) {
                  final spotNum = int.tryParse(a.spotId.replaceFirst('Park', '')) ?? 0;
                  if (spotNum == index + 1) {
                    alert = a;
                    break;
                  }
                }

                final hasAlert = alert != null;
                final spotLabel = 'P${(index + 1).toString().padLeft(3, '0')}';

                return Container(
                  decoration: BoxDecoration(
                    color: hasAlert
                        ? AppColors.danger.withOpacity(0.2)
                        : AppColors.success.withOpacity(0.1),
                    borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                    border: Border.all(
                      color: hasAlert
                          ? AppColors.danger.withOpacity(0.5)
                          : AppColors.success.withOpacity(0.3),
                    ),
                  ),
                  child: Stack(
                    children: [
                      Center(
                        child: Text(
                          spotLabel,
                          style: TextStyle(
                            fontSize: 14,
                            fontWeight: FontWeight.w700,
                            color: hasAlert ? AppColors.danger : AppColors.success,
                          ),
                        ),
                      ),
                      if (hasAlert)
                        Positioned(
                          top: 4,
                          right: 4,
                          child: Container(
                            width: 10,
                            height: 10,
                            decoration: const BoxDecoration(
                              color: AppColors.danger,
                              shape: BoxShape.circle,
                            ),
                          ),
                        ),
                    ],
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  void _showSnackBar(String message) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message)),
    );
  }
}

class _SlidableAlertItem extends StatefulWidget {
  final AlertModel alert;
  final VoidCallback? onDispatch;
  final VoidCallback? onNotify;
  final VoidCallback? onResolve;

  const _SlidableAlertItem({
    required this.alert,
    this.onDispatch,
    this.onNotify,
    this.onResolve,
  });

  @override
  State<_SlidableAlertItem> createState() => _SlidableAlertItemState();
}

class _SlidableAlertItemState extends State<_SlidableAlertItem> {
  double _dragOffset = 0;
  bool _isDragging = false;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 12),
      height: 100,
      child: Stack(
        children: [
          _buildActionButtons(),
          GestureDetector(
            onHorizontalDragStart: (_) {
              setState(() => _isDragging = true);
            },
            onHorizontalDragUpdate: (details) {
              setState(() {
                _dragOffset += details.delta.dx;
                if (_dragOffset > 0) _dragOffset = 0;
                if (_dragOffset < -150) _dragOffset = -150;
              });
            },
            onHorizontalDragEnd: (_) {
              setState(() {
                _isDragging = false;
                if (_dragOffset > -50) _dragOffset = 0;
              });
            },
            child: Transform.translate(
              offset: Offset(_dragOffset, 0),
              child: _buildAlertContent(),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildActionButtons() {
    return Row(
      children: [
        Expanded(
          child: GestureDetector(
            onTap: widget.onDispatch,
            child: Container(
              height: 100,
              color: AppColors.primary,
              alignment: Alignment.center,
              child: const Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.send, color: Colors.white, size: 24),
                  SizedBox(height: 4),
                  Text('派单', style: TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.w600)),
                ],
              ),
            ),
          ),
        ),
        Expanded(
          child: GestureDetector(
            onTap: widget.onNotify,
            child: Container(
              height: 100,
              color: AppColors.warning,
              alignment: Alignment.center,
              child: const Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.notification_important, color: Colors.white, size: 24),
                  SizedBox(height: 4),
                  Text('通知', style: TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.w600)),
                ],
              ),
            ),
          ),
        ),
        Expanded(
          child: GestureDetector(
            onTap: widget.onResolve,
            child: Container(
              height: 100,
              color: AppColors.success,
              alignment: Alignment.center,
              child: const Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.check_circle, color: Colors.white, size: 24),
                  SizedBox(height: 4),
                  Text('处置', style: TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.w600)),
                ],
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildAlertContent() {
    final alert = widget.alert;
    final statusBadge = StatusBadge.fromStatus(alert.status);

    return CardContainer(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Container(
              width: 4,
              height: 60,
              decoration: BoxDecoration(
                color: alert.status == 'pending' ? AppColors.danger : AppColors.warning,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                mainAxisSize: MainAxisSize.min,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          alert.plateNumber,
                          style: const TextStyle(
                            fontSize: 16,
                            fontWeight: FontWeight.w700,
                            color: AppColors.textPrimary,
                          ),
                        ),
                      ),
                      const SizedBox(width: 8),
                      statusBadge,
                    ],
                  ),
                  const SizedBox(height: 6),
                  Text(
                    '车位 ${alert.spotId} · 占用 ${alert.occupiedHours}小时',
                    style: const TextStyle(
                      fontSize: 13,
                      color: AppColors.textSecondary,
                    ),
                  ),
                  if (alert.createdAt != null) ...[
                    const SizedBox(height: 4),
                    Text(
                      '创建于 ${_formatTime(alert.createdAt!)}',
                      style: const TextStyle(
                        fontSize: 11,
                        color: AppColors.textSecondary,
                      ),
                    ),
                  ],
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: AppColors.textSecondary),
          ],
        ),
      ),
    );
  }

  String _formatTime(DateTime time) {
    return '${time.month}月${time.day}日 ${time.hour.toString().padLeft(2, '0')}:${time.minute.toString().padLeft(2, '0')}';
  }
}
