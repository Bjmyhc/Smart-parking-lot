import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/providers/parking_provider.dart';

class AlertsPage extends StatefulWidget {
  const AlertsPage({super.key});

  @override
  State<AlertsPage> createState() => _AlertsPageState();
}

class _AlertsPageState extends State<AlertsPage> {
  int _filterIndex = 0;

  List<AlertModel> _filteredAlerts(List<AlertModel> alerts) {
    switch (_filterIndex) {
      case 1:
        return alerts.where((a) => a.status == 'pending').toList();
      case 2:
        return alerts.where((a) => a.status == 'resolved').toList();
      default:
        return alerts;
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();
    final alerts = provider.alerts;
    return Scaffold(
      backgroundColor: AppColors.background,
      body: Column(
        children: [
          _buildHeader(),
          _buildFilterTabs(),
          Expanded(
            child: provider.isLoading && alerts.isEmpty
                ? const Center(child: CircularProgressIndicator())
                : _buildBody(alerts, provider),
          ),
        ],
      ),
    );
  }

  Widget _buildHeader() {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16,
        left: AppDims.paddingPage,
        right: AppDims.paddingPage,
        bottom: 16,
      ),
      child: Row(
        children: [
          const Expanded(
            child: Padding(
              padding: EdgeInsets.only(left: 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    '告警管理',
                    style: TextStyle(
                      fontSize: 28,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                ],
              ),
            ),
          ),
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.notifications_none, color: AppColors.textSecondary, size: 22),
          ),
        ],
      ),
    );
  }

  Widget _buildFilterTabs() {
    final tabs = ['全部', '待处理', '已处理'];
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppDims.paddingPage),
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        child: Row(
          children: List.generate(tabs.length, (i) {
            final isSelected = _filterIndex == i;
            return Container(
              margin: EdgeInsets.only(right: i < tabs.length - 1 ? 10 : 0),
              child: GestureDetector(
                onTap: () {
                  setState(() {
                    _filterIndex = i;
                  });
                },
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
                  decoration: BoxDecoration(
                    color: isSelected ? AppColors.primary : Colors.white,
                    borderRadius: BorderRadius.circular(AppDims.radiusLarge),
                    border: Border.all(
                      color: isSelected ? AppColors.primary : AppColors.textSecondary.withOpacity(0.2),
                      width: 1,
                    ),
                  ),
                  child: Text(
                    tabs[i],
                    style: TextStyle(
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      color: isSelected ? Colors.white : AppColors.textPrimary,
                    ),
                  ),
                ),
              ),
            );
          }),
        ),
      ),
    );
  }

  Widget _buildBody(List<AlertModel> alerts, ParkingProvider provider) {
    final list = _filteredAlerts(alerts);
    if (list.isEmpty) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
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
      onRefresh: provider.refresh,
      child: ListView.builder(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        itemCount: list.length,
        itemBuilder: (context, index) {
          return _buildAlertCard(list[index]);
        },
      ),
    );
  }

  Widget _buildAlertCard(AlertModel alert) {
    Color iconBgColor;
    Color iconColor;
    IconData iconData;

    switch (alert.status) {
      case 'pending':
        iconBgColor = AppColors.danger.withOpacity(0.1);
        iconColor = AppColors.danger;
        iconData = Icons.warning_amber;
        break;
      case 'resolved':
      default:
        iconBgColor = AppColors.success.withOpacity(0.1);
        iconColor = AppColors.success;
        iconData = Icons.check_circle;
        break;
    }

    return Container(
      margin: const EdgeInsets.only(bottom: AppDims.gapCard),
      child: CardContainer(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Container(
                  width: 56,
                  height: 56,
                  decoration: BoxDecoration(
                    color: iconBgColor,
                    borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                  ),
                  child: Icon(iconData, color: iconColor, size: 30),
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
                              '僵尸车告警 - ${alert.plateNumber}',
                              style: const TextStyle(
                                fontSize: 16,
                                fontWeight: FontWeight.w700,
                                color: AppColors.textPrimary,
                              ),
                            ),
                          ),
                          const SizedBox(width: 8),
                          StatusBadge.fromStatus(alert.status),
                        ],
                      ),
                      const SizedBox(height: 8),
                      Text(
                        '车位: ${alert.spotId} · 占用 ${alert.occupiedHours}小时',
                        style: const TextStyle(
                          fontSize: 13,
                          color: AppColors.textSecondary,
                        ),
                      ),
                      const SizedBox(height: 4),
                      if (alert.createdAt != null)
                        Text(
                          '创建时间: ${_formatDate(alert.createdAt!)}',
                          style: const TextStyle(
                            fontSize: 12,
                            color: AppColors.textSecondary,
                          ),
                        ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 16),
            Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                _buildIgnoreButton(alert),
                if (alert.status == 'pending') ...[
                  const SizedBox(width: 12),
                  _buildProcessButton(alert),
                ],
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildIgnoreButton(AlertModel alert) {
    return GestureDetector(
      onTap: () async {
        final provider = context.read<ParkingProvider>();
        final confirmed = await showModalBottomSheet<bool>(
          context: context,
          backgroundColor: Colors.transparent,
          isScrollControlled: true,
          builder: (ctx) => Container(
            decoration: const BoxDecoration(
              color: Colors.white,
              borderRadius: BorderRadius.vertical(top: Radius.circular(24)),
            ),
            padding: EdgeInsets.only(
              left: 24,
              right: 24,
              top: 20,
              bottom: MediaQuery.of(ctx).padding.bottom + 20,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Container(
                  width: 40,
                  height: 4,
                  decoration: BoxDecoration(
                    color: AppColors.textSecondary.withOpacity(0.2),
                    borderRadius: BorderRadius.circular(2),
                  ),
                ),
                const SizedBox(height: 20),
                const Text(
                  '确认忽略',
                  style: TextStyle(
                    fontSize: 18,
                    fontWeight: FontWeight.w700,
                    color: AppColors.textPrimary,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  '确定要忽略该告警吗？忽略后将不再显示此条僵尸车告警。',
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    fontSize: 14,
                    color: AppColors.textSecondary,
                    height: 1.5,
                  ),
                ),
                const SizedBox(height: 28),
                GestureDetector(
                  onTap: () => Navigator.pop(ctx, true),
                  child: Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(vertical: 15),
                    decoration: BoxDecoration(
                      color: AppColors.primary,
                      borderRadius: BorderRadius.circular(14),
                    ),
                    child: const Text(
                      '确认忽略',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                        color: Colors.white,
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 12),
                GestureDetector(
                  onTap: () => Navigator.pop(ctx, false),
                  child: Container(
                    width: double.infinity,
                    padding: const EdgeInsets.symmetric(vertical: 15),
                    decoration: BoxDecoration(
                      color: AppColors.textSecondary.withOpacity(0.08),
                      borderRadius: BorderRadius.circular(14),
                    ),
                    child: const Text(
                      '取消',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 15,
                        fontWeight: FontWeight.w500,
                        color: AppColors.textSecondary,
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        );
        if (confirmed == true) {
          _showSnackBar('已忽略');
          provider.ignoreAlert(alert);
        }
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.textSecondary.withOpacity(0.08),
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: const [
            Icon(Icons.not_interested, color: AppColors.textSecondary, size: 18),
            SizedBox(width: 6),
            Text(
              '忽略',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: AppColors.textSecondary,
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildProcessButton(AlertModel alert) {
    return GestureDetector(
      onTap: () {
        context.read<ParkingProvider>().resolveAlert(alert);
        _showSnackBar('已处置');
      },
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
        decoration: BoxDecoration(
          color: AppColors.primary,
          borderRadius: BorderRadius.circular(AppDims.radiusMedium),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: const [
            Icon(Icons.check, color: Colors.white, size: 18),
            SizedBox(width: 6),
            Text(
              '处理',
              style: TextStyle(
                fontSize: 13,
                fontWeight: FontWeight.w600,
                color: Colors.white,
              ),
            ),
          ],
        ),
      ),
    );
  }

  String _formatDate(DateTime time) {
    return '${time.year}-${time.month}-${time.day}';
  }

  void _showSnackBar(String message) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message)),
    );
  }
}
