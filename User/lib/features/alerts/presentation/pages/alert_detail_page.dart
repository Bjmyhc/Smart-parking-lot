import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import '../../../../core/models/alert_model.dart';
import '../../../../core/models/spot_model.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../spots/presentation/pages/spot_detail_page.dart';

/// 🆕 独立告警详情页 (工单风格, 与车位实时详情解耦)
/// 设计亮点:
/// 1. 顶部渐变状态横幅 (按告警状态变色 + 智能倒计时/耗时统计)
/// 2. 停车小票风格车辆信息卡 (虚线边框 + 撕票口 + 仿真车牌展示)
/// 3. 带阶段高亮的处理时间轴 (当前阶段脉冲阴影 + 已处理完成印章)
/// 4. 浮动底部操作栏 (按状态显示对应动作, 不干扰内容)
class AlertDetailPage extends StatefulWidget {
  final AlertModel alert;

  const AlertDetailPage({super.key, required this.alert});

  @override
  State<AlertDetailPage> createState() => _AlertDetailPageState();
}

class _AlertDetailPageState extends State<AlertDetailPage> {
  Timer? _countdownTimer;
  Duration? _remainingDispatchWait; // 距自动派单剩余时间

  @override
  void initState() {
    super.initState();
    // ⭐ 1s UI 刷新定时器: 让「已通知」状态的派单倒计时秒数持续跳动,
    //   并随状态变化自动切换显示/隐藏。无论进页面时是哪种状态都启动,
    //   这样 pending→点通知变 notified→倒计时立即开始, 不必退出重进。
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _countdownTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        if (!mounted) return;
        final p = context.read<ParkingProvider>();
        final a = p.allAlerts.firstWhere((x) => x.id == widget.alert.id, orElse: () => widget.alert);
        // 已归档: 停掉定时器, 不再跳动
        if (a.status == 'resolved') {
          _countdownTimer?.cancel();
          _countdownTimer = null;
          setState(() => _remainingDispatchWait = null);
          return;
        }
        // 只在「已通知」状态显示派单倒计时, 其它状态清空
        setState(() {
          _remainingDispatchWait = a.status == 'notified' ? _calcRemaining(a, p) : null;
        });
      });
    });
  }

  @override
  void dispose() {
    _countdownTimer?.cancel();
    super.dispose();
  }

  /// ⭐ 计算: 通知时间到现在, 距离派单阈值还剩多久
  ///   - 参数改为 alert 对象, 供 build 每次重建时传「最新实时实例」, 不再用入参快照
  Duration? _calcRemaining(AlertModel alert, ParkingProvider provider) {
    final notifiedAt = provider.getNotifiedAt(alert.id) ?? alert.createdAt?.add(const Duration(seconds: 30));
    if (notifiedAt == null) return null;
    final waitMs = provider.policy.dispatchWaitSec * 1000;
    final target = notifiedAt.add(Duration(milliseconds: waitMs));
    final diff = target.difference(DateTime.now());
    return diff.isNegative ? Duration.zero : diff;
  }

  /* ==================== 顶部状态横幅 (⭐ 已整合返回按钮, 整体对齐更规整) ==================== */
  Widget _buildStatusBanner(AlertModel alert, ParkingProvider provider) {
    final status = alert.status;
    String title, subtitle;
    Color bgStart, bgEnd;
    IconData icon;

    switch (status) {
      case 'pending':
        title = '待处理';
        subtitle = '检测到僵尸车，请立即通知车主挪车';
        bgStart = const Color(0xFFFFB347);
        bgEnd = const Color(0xFFFF8C00);
        icon = Icons.warning_amber_rounded;
        break;
      case 'notified':
        title = '已通知车主';
        final r = _remainingDispatchWait;
        if (r == null) {
          subtitle = '等待车主挪车中…';
        } else if (r.inSeconds <= 0) {
          subtitle = '已达到派单阈值，系统即将自动派单';
        } else {
          subtitle = '距自动派单还剩 ${r.inMinutes > 0 ? "${r.inMinutes}分${r.inSeconds % 60}秒" : "${r.inSeconds}秒"}';
        }
        bgStart = const Color(0xFF2196F3);
        bgEnd = const Color(0xFF1565C0);
        icon = Icons.notifications_active_rounded;
        break;
      case 'dispatched':
        title = '处理中';
        final dispatchedAt = provider.getDispatchedAt(alert.id);
        final handler = provider.getHandlerName(alert.id);
        final elapsedStr = dispatchedAt == null
            ? ''
            : ' · 已派单 ${_formatDurationShort(DateTime.now().difference(dispatchedAt).inSeconds)}';
        subtitle = '处理人：${handler ?? "张师傅"}$elapsedStr';
        bgStart = const Color(0xFFFF7043);
        bgEnd = const Color(0xFFD84315);
        icon = Icons.assignment_ind_rounded;
        break;
      case 'resolved':
      default:
        title = '已处理完成';
        final resolvedInfo = _calcResolutionStats(alert, provider);
        subtitle = resolvedInfo;
        bgStart = const Color(0xFF43A047);
        bgEnd = const Color(0xFF1B5E20);
        icon = Icons.check_circle_rounded;
    }

    return Container(
      width: double.infinity,
      decoration: BoxDecoration(
        gradient: LinearGradient(
          colors: [bgStart, bgEnd],
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
        ),
      ),
      child: SafeArea(
        bottom: false,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(12, 8, 20, 24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              /* 第一行: 返回按钮 + 告警单号 → 左右对齐 */
              Row(
                children: [
                  IconButton(
                    onPressed: () => Navigator.pop(context),
                    icon: const Icon(Icons.arrow_back_ios, color: Colors.white, size: 20),
                    padding: EdgeInsets.zero,
                    constraints: const BoxConstraints(minWidth: 36, minHeight: 36),
                    tooltip: '返回',
                  ),
                  const Spacer(),
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                    decoration: BoxDecoration(
                      color: Colors.white.withValues(alpha: 0.18),
                      borderRadius: BorderRadius.circular(20),
                    ),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Icon(Icons.receipt_long, size: 13, color: Colors.white.withValues(alpha: 0.9)),
                        const SizedBox(width: 6),
                        Text(
                          widget.alert.id.toUpperCase(),  // 工单号只是展示文本, id不会变, 用快照没问题
                          style: TextStyle(
                            fontSize: 12,
                            color: Colors.white.withValues(alpha: 0.92),
                            fontFamily: 'monospace',
                            letterSpacing: 0.5,
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 4),
              /* 第二行: 图标 + 标题/副标题 (左对齐完全一致) */
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const SizedBox(width: 4),
                  Container(
                    width: 56,
                    height: 56,
                    decoration: BoxDecoration(
                      color: Colors.white.withValues(alpha: 0.2),
                      borderRadius: BorderRadius.circular(16),
                    ),
                    child: Icon(icon, color: Colors.white, size: 32),
                  ),
                  const SizedBox(width: 16),
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.only(top: 2),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            title,
                            style: const TextStyle(
                              fontSize: 24,
                              fontWeight: FontWeight.bold,
                              color: Colors.white,
                              height: 1.1,
                            ),
                          ),
                          const SizedBox(height: 8),
                          Text(
                            subtitle,
                            style: TextStyle(
                              fontSize: 13.5,
                              color: Colors.white.withValues(alpha: 0.92),
                              height: 1.4,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  /// 已处理工单的效率统计 (通知耗时 + 派单耗时 + 总耗时)
  /// ⭐ 参数使用 alert (实时实例), 不再读 widget.alert 入参快照 → 自动派单/手动派单后立即刷新显示
  String _calcResolutionStats(AlertModel alert, ParkingProvider provider) {
    final alertId = alert.id;
    final createdAt = alert.createdAt;
    final notifiedAt = provider.getNotifiedAt(alertId);
    final dispatchedAt = provider.getDispatchedAt(alertId);
    final handledAt = provider.getHandledAt(alertId);

    if (createdAt == null || handledAt == null) return '工单已归档';
    final total = handledAt.difference(createdAt);
    final parts = <String>[];
    if (notifiedAt != null) {
      parts.add('通知耗时 ${_formatDurationShort(notifiedAt.difference(createdAt).inSeconds)}');
    }
    if (dispatchedAt != null && notifiedAt != null) {
      parts.add('派单等待 ${_formatDurationShort(dispatchedAt.difference(notifiedAt).inSeconds)}');
    }
    parts.add('总耗时 ${_formatDurationShort(total.inSeconds)}');
    return parts.join(' · ');
  }

  /* ==================== 车辆信息卡 (停车小票风格) ==================== */
  Widget _buildVehicleReceiptCard(AlertModel alert, ParkingProvider provider) {
    final spotsById = {for (final s in provider.spots) s.id: s};
    final spot = spotsById[alert.spotId];
    /* ⭐⭐⭐ 关键修复: 占用时长按【工单状态】区分数据源, 实现事件级隔离
     *   - 未结束工单 (pending / notified / dispatched): 用实时 actualOccupiedSec → 秒数自动增长
     *   - 已结束工单 (resolved): 用 alert.occupiedSec 固化的最终值 → 不再读该车位当前状态,
     *     避免同一车位新停一辆车时, 把新车秒数"串"到旧工单上 */
    final isResolved = alert.status == 'resolved';
    final realOcc = isResolved
        ? alert.occupiedSec
        : (spot?.actualOccupiedSec ?? alert.occupiedSec);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppDims.paddingPage),
      child: Container(
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: const Color(0xFFE5E7EB), width: 1),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.05),
              blurRadius: 16,
              offset: const Offset(0, 4),
            ),
          ],
        ),
        child: Stack(
          children: [
            /* 左右两侧撕票口半圆 */
            Positioned(
              left: -8,
              top: 70,
              child: Container(width: 16, height: 16, decoration: const BoxDecoration(color: AppColors.background, shape: BoxShape.circle)),
            ),
            Positioned(
              right: -8,
              top: 70,
              child: Container(width: 16, height: 16, decoration: const BoxDecoration(color: AppColors.background, shape: BoxShape.circle)),
            ),
            /* 中间虚线分隔 */
            Positioned(
              left: 12,
              right: 12,
              top: 78,
              child: CustomPaint(
                size: const Size(double.infinity, 1),
                painter: _DashedLinePainter(),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(18, 16, 18, 18),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  /* 小票标题 */
                  Row(
                    children: [
                      Container(
                        width: 28,
                        height: 28,
                        decoration: BoxDecoration(
                          color: AppColors.primary.withValues(alpha: 0.12),
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: const Icon(Icons.local_parking, color: AppColors.primary, size: 18),
                      ),
                      const SizedBox(width: 10),
                      const Text(
                        '僵尸车检测 · 停车告警单',
                        style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                      ),
                      const Spacer(),
                      Text(
                        alert.createdAt != null
                            ? _formatDateOnly(alert.createdAt!)
                            : '',
                        style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
                      ),
                    ],
                  ),
                  const SizedBox(height: 22),
                  /* 仿真车牌展示 */
                  Center(
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 26, vertical: 10),
                      decoration: BoxDecoration(
                        gradient: const LinearGradient(
                          colors: [Color(0xFF1A6FFF), Color(0xFF0052D4)],
                          begin: Alignment.topCenter,
                          end: Alignment.bottomCenter,
                        ),
                        borderRadius: BorderRadius.circular(6),
                        boxShadow: [
                          BoxShadow(
                            color: const Color(0xFF0052D4).withValues(alpha: 0.25),
                            blurRadius: 10,
                            offset: const Offset(0, 3),
                          ),
                        ],
                        border: Border.all(color: Colors.white.withValues(alpha: 0.5), width: 1.5),
                      ),
                      child: Text(
                        alert.plateNumber,
                        style: const TextStyle(
                          fontSize: 28,
                          fontWeight: FontWeight.bold,
                          color: Colors.white,
                          letterSpacing: 4,
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(height: 22),
                  /* 小票信息列表 */
                  _buildReceiptRow(Icons.location_on_rounded, '车位编号', alert.spotId),
                  _buildReceiptRow(Icons.timer_outlined, '占用时长', SpotModel.formatOccupiedDuration(realOcc)),
                  _buildReceiptRow(Icons.access_time_rounded, '检测时间', alert.createdAt != null ? _formatDateTime(alert.createdAt!) : '未知'),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildReceiptRow(IconData icon, String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 7),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          Icon(icon, size: 17, color: AppColors.textSecondary),
          const SizedBox(width: 10),
          /* ⭐ 标题固定宽80dp, 彻底解决错位: 不管 label 字数多少都占相同宽度, value 自动靠右 */
          SizedBox(
            width: 80,
            child: Text(
              label,
              style: const TextStyle(fontSize: 14, color: AppColors.textSecondary),
            ),
          ),
          Expanded(
            child: Text(
              value,
              style: const TextStyle(fontSize: 14, color: AppColors.textPrimary, fontWeight: FontWeight.w500),
              textAlign: TextAlign.right,
            ),
          ),
        ],
      ),
    );
  }

  /* ==================== 处理情况时间轴 (工单阶段) ==================== */
  Widget _buildHandlingTimeline(AlertModel alert, ParkingProvider provider) {
    // ⭐ 读取时间戳用 alertId（事件级），不是车位id → 同一车位多个独立工单时间线互不干扰
    final alertId = alert.id;
    final step1At = alert.createdAt;
    final step2At = provider.getNotifiedAt(alertId);
    final step3At = provider.getDispatchedAt(alertId);
    final step4At = provider.getHandledAt(alertId);
    final handler = provider.getHandlerName(alertId);
    final status = alert.status;

    final done1 = step1At != null;
    final done2 = step2At != null;
    final done3 = step3At != null;
    final done4 = step4At != null;

    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: AppDims.paddingPage),
      child: CardContainer(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Text(
                  '处理情况',
                  style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
                ),
                const Spacer(),
                /* 已处理完成印章 */
                if (status == 'resolved')
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                    decoration: BoxDecoration(
                      border: Border.all(color: AppColors.success, width: 1.8),
                      borderRadius: BorderRadius.circular(6),
                      color: AppColors.success.withValues(alpha: 0.06),
                    ),
                    transform: Matrix4.rotationZ(-0.08),
                    child: Text(
                      '已完成',
                      style: TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: AppColors.success.withValues(alpha: 0.9)),
                    ),
                  ),
              ],
            ),
            const SizedBox(height: 20),
            _buildStep(
              title: '告警上报',
              subtitle: '系统检测到僵尸车，自动生成告警工单',
              time: step1At,
              done: done1,
              highlight: status == 'pending',
              isLast: false,
              icon: Icons.warning_amber,
            ),
            _buildStep(
              title: '通知车主',
              subtitle: done2 ? '已发送挪车提醒短信' : '等待通知车主',
              time: step2At,
              done: done2,
              highlight: status == 'notified',
              isLast: false,
              icon: Icons.notifications_active,
            ),
            _buildStep(
              title: '工单派单',
              subtitle: done3 && handler != null ? '派单给 $handler 现场处理' : '等待派单处理',
              time: step3At,
              done: done3,
              highlight: status == 'dispatched',
              isLast: false,
              icon: Icons.assignment_turned_in,
            ),
            _buildStep(
              title: '处理完成',
              subtitle: done4 ? '僵尸车已处理完毕，工单归档' : '处理进行中',
              time: step4At,
              done: done4,
              highlight: false,
              isLast: true,
              icon: Icons.check_circle,
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildStep({
    required String title,
    required String subtitle,
    required DateTime? time,
    required bool done,
    required bool highlight,
    required bool isLast,
    required IconData icon,
  }) {
    final bgColor = done ? AppColors.success : AppColors.textSecondary.withValues(alpha: 0.15);
    final iconColor = done ? Colors.white : AppColors.textSecondary;
    final lineColor = done ? AppColors.success : AppColors.textSecondary.withValues(alpha: 0.2);

    return SizedBox(
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 32,
            child: Column(
              children: [
                Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: bgColor,
                    shape: BoxShape.circle,
                    boxShadow: highlight
                        ? [BoxShadow(color: AppColors.warning.withValues(alpha: 0.5), blurRadius: 10, spreadRadius: 2)]
                        : null,
                  ),
                  child: Icon(done ? Icons.check : icon, color: iconColor, size: 18),
                ),
                if (!isLast)
                  Container(width: 2, height: 48, color: lineColor, margin: const EdgeInsets.symmetric(vertical: 6)),
              ],
            ),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Padding(
              padding: EdgeInsets.only(top: 2, bottom: isLast ? 0 : 20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          title,
                          style: TextStyle(
                            fontSize: 15,
                            fontWeight: highlight ? FontWeight.bold : FontWeight.w600,
                            color: highlight ? AppColors.warning : (done ? AppColors.textPrimary : AppColors.textSecondary),
                          ),
                        ),
                      ),
                      if (time != null)
                        Text(
                          _formatDateTime(time),
                          style: const TextStyle(fontSize: 12, color: AppColors.textSecondary),
                        ),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(subtitle, style: const TextStyle(fontSize: 13, color: AppColors.textSecondary)),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  /* ==================== 底部浮动操作栏 ==================== */
  Widget _buildBottomActionBar(AlertModel alert, ParkingProvider provider) {
    final status = alert.status;
    // ParkingProvider没有spotsById getter → 实时从spots列表转成ID->Spot的Map
    final spotsById = {for (final s in provider.spots) s.id: s};
    final spot = spotsById[alert.spotId];

    Widget primary;
    Widget? secondary;

    switch (status) {
      case 'pending':
        primary = SizedBox(
          width: double.infinity,
          height: 48,
          child: ElevatedButton.icon(
            icon: const Icon(Icons.notifications_active, size: 20),
            label: const Text('立即通知车主', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
            /* ⭐ 配色和顶部橙横幅一致: 明亮橙色, 大圆角+阴影 提亮显精神 */
            style: ElevatedButton.styleFrom(
              backgroundColor: AppColors.warning,
              foregroundColor: Colors.white,
              elevation: 3,
              shadowColor: AppColors.warning.withValues(alpha: 0.4),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
            ),
            onPressed: () async {
              if (spot == null) return;
              await provider.notifyOwner(spot);
              if (mounted) setState(() {});
            },
          ),
        );
        break;
      case 'notified':
        primary = SizedBox(
          width: double.infinity,
          height: 48,
          child: ElevatedButton.icon(
            icon: const Icon(Icons.assignment_turned_in, size: 20),
            label: const Text('立即派单处理', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
            /* ⭐ 配色和顶部蓝横幅 + 次按钮边框一致: 统一明亮蓝色, 不再红+蓝撞色 */
            style: ElevatedButton.styleFrom(
              backgroundColor: AppColors.primary,
              foregroundColor: Colors.white,
              elevation: 3,
              shadowColor: AppColors.primary.withValues(alpha: 0.4),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
            ),
            onPressed: () async {
              if (spot == null) return;
              await provider.dispatchSpot(spot, handlerName: '张师傅');
              if (mounted) setState(() {});
            },
          ),
        );
        secondary = _buildViewSpotButton(spot);
        break;
      case 'dispatched':
      case 'resolved':
      default:
        primary = SizedBox(
          width: double.infinity,
          child: _buildViewSpotButton(spot) ?? const SizedBox.shrink(),
        );
    }

    return Positioned(
      left: 0,
      right: 0,
      bottom: 0,
      child: Container(
        padding: EdgeInsets.fromLTRB(
          AppDims.paddingPage,
          12,
          AppDims.paddingPage,
          MediaQuery.of(context).padding.bottom + 12,
        ),
        decoration: BoxDecoration(
          color: Colors.white,
          boxShadow: [BoxShadow(color: Colors.black.withValues(alpha: 0.06), blurRadius: 12, offset: const Offset(0, -2))],
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            primary,
            if (secondary != null) ...[const SizedBox(height: 10), secondary],
          ],
        ),
      ),
    );
  }

  Widget? _buildViewSpotButton(SpotModel? spot) {
    if (spot == null) return null;
    return SizedBox(
      width: double.infinity,
      height: 48,
      child: OutlinedButton.icon(
        icon: const Icon(Icons.location_on_outlined, size: 20),
        label: const Text('查看车位实时状态', style: TextStyle(fontSize: 15, fontWeight: FontWeight.w600)),
        /* ⭐ 和主按钮统一风格: 大圆角 + 蓝色边框 + 蓝色文字, 不会再显得生硬 */
        style: OutlinedButton.styleFrom(
          foregroundColor: AppColors.primary,
          side: BorderSide(color: AppColors.primary.withValues(alpha: 0.6), width: 1.3),
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
        ),
        onPressed: () {
          Navigator.push(
            context,
            MaterialPageRoute(builder: (_) => SpotDetailPage(spot: spot)),
          );
        },
      ),
    );
  }

  /* ==================== 工具方法 ==================== */
  String _formatDurationShort(int sec) => SpotModel.formatOccupiedDuration(sec);
  String _formatDateOnly(DateTime d) => '${d.year}-${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')}';
  String _formatDateTime(DateTime d) {
    return '${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')} '
        '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<ParkingProvider>(
      builder: (context, provider, _) {
        // ⭐ 用 id 从 provider 取【实时 alert】, 不再用 widget.alert 入参快照
        //   → 点击派单/通知后 notifyListeners 触发重建, 拿到的就是最新状态
        //   → 页面停留期间 3s 轮询更新车位, allAlerts 重新生成, 同步刷新
        final live = provider.allAlerts.firstWhere(
          (a) => a.id == widget.alert.id,
          orElse: () => widget.alert,
        );
        return Scaffold(
          backgroundColor: AppColors.background,
          /* ⭐ 外层Stack只保留给浮动底部栏, 顶部返回按钮已整合进横幅内部, 消除叠层混乱 */
          body: Stack(
            children: [
              SingleChildScrollView(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    _buildStatusBanner(live, provider),
                    const SizedBox(height: 20),
                    _buildVehicleReceiptCard(live, provider),
                    const SizedBox(height: AppDims.gapCard),
                    _buildHandlingTimeline(live, provider),
                    const SizedBox(height: 120),
                  ],
                ),
              ),
              _buildBottomActionBar(live, provider),
            ],
          ),
        );
      },
    );
  }
}

/* ==================== 辅助类: 小票虚线分隔 ==================== */
class _DashedLinePainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = const Color(0xFFE5E7EB)
      ..strokeWidth = 1
      ..style = PaintingStyle.stroke;
    const dash = 5.0;
    const gap = 4.0;
    double x = 0;
    while (x < size.width) {
      canvas.drawLine(Offset(x, 0), Offset(x + dash, 0), paint);
      x += dash + gap;
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
