import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';
import '../services/api_service.dart';

/// 全局数据源 (单一数据层): 唯一持有车位数据 + 唯一的轮询刷新定时器 +
/// 真实/本地模式 + 告警派生与处理动作.
///
/// 重构前每个页面各自 new ApiService() 并各自开 3s 定时器, 同一批数据被
/// 重复请求、互不相通; 现在所有页面统一 context.watch<ParkingProvider>(),
/// 任何页面/动作修改数据后, 其它页面通过 notifyListeners 实时感知.
class ParkingProvider extends ChangeNotifier {
  ParkingProvider({ApiService? apiService})
      : _apiService = apiService ?? ApiService() {
    _init();
  }

  static const _modeKey = 'spots_real_mode';

  final ApiService _apiService;
  Timer? _refreshTimer;
  bool _refreshing = false;

  List<SpotModel> _spots = [];
  bool _realOnly = false; // false=本地模式(真实+模拟), true=真实模式(仅真实设备)
  bool _isLoading = true;

  /* 告警处理记录 (按车位 spotId): 忽略/已处理的告警在 3s 刷新后保持状态 */
  final Set<String> _ignoredAlertIds = {};
  final Set<String> _resolvedAlertIds = {};

  /* 一周趋势为演示用静态数据 (图表占位, 非平台真实统计) */
  static final List<DailyTrend> _mockWeeklyTrend = [
    DailyTrend(date: '周一', avgOccupancy: 45, alertsCount: 2),
    DailyTrend(date: '周二', avgOccupancy: 52, alertsCount: 1),
    DailyTrend(date: '周三', avgOccupancy: 48, alertsCount: 3),
    DailyTrend(date: '周四', avgOccupancy: 60, alertsCount: 0),
    DailyTrend(date: '周五', avgOccupancy: 55, alertsCount: 2),
    DailyTrend(date: '周六', avgOccupancy: 68, alertsCount: 1),
    DailyTrend(date: '周日', avgOccupancy: 58, alertsCount: 2),
  ];

  /// 启动流程: 先加载持久化的模式, 再首次拉取, 最后启动全局轮询.
  Future<void> _init() async {
    await _loadMode();
    await refresh();
    _refreshTimer = Timer.periodic(const Duration(seconds: 3), (_) => refresh());
  }

  Future<void> _loadMode() async {
    final prefs = await SharedPreferences.getInstance();
    _realOnly = prefs.getBool(_modeKey) ?? false;
  }

  /* ==================== 只读数据 ==================== */

  List<SpotModel> get spots => List.unmodifiable(_spots);
  bool get realOnly => _realOnly;
  bool get isLoading => _isLoading;

  int get totalSpots => _spots.length;
  List<SpotModel> get freeSpots => _spots.where((s) => s.isFree).toList();
  List<SpotModel> get occupiedSpots => _spots.where((s) => s.isOccupied).toList();
  List<SpotModel> get zombieSpots => _spots.where((s) => s.isZombie).toList();
  int get freeCount => freeSpots.length;
  int get occupiedCount => _spots.where((s) => s.status == 'occupied').length;
  int get zombieCount => zombieSpots.length;

  /// 告警由车位状态派生 (占用 >= 24h 即为僵尸车告警), 忽略/已处理不重复出现.
  List<AlertModel> get alerts {
    final list = <AlertModel>[];
    for (final spot in _spots) {
      if (!spot.isOccupied || spot.occupiedHours < 24) continue;
      if (_ignoredAlertIds.contains(spot.id)) continue;
      list.add(AlertModel(
        id: 'alert_${spot.id}',
        plateNumber: spot.plateNumber ?? '未知车牌',
        spotId: spot.id,
        occupiedHours: spot.occupiedHours,
        status: _resolvedAlertIds.contains(spot.id)
            ? 'resolved'
            : (spot.occupiedHours >= 72 ? 'pending' : 'dispatched'),
        createdAt: DateTime.now().subtract(Duration(hours: spot.occupiedHours)),
      ));
    }
    list.sort((a, b) => b.occupiedHours.compareTo(a.occupiedHours));
    return list;
  }

  /// 统计模型: 占用率/僵尸车数由 spots 派生, 一周趋势为演示数据.
  StatsModel get stats => StatsModel(
        totalSpots: totalSpots,
        occupiedSpots: occupiedCount,
        zombieSpots: zombieCount,
        occupancyRate: totalSpots > 0 ? occupiedCount / totalSpots : 0.0,
        weeklyTrend: _mockWeeklyTrend,
      );

  /* ==================== 数据刷新 ==================== */

  /// 唯一的数据拉取入口: 从 OneNET 拉取 (按当前模式过滤), 失败保留上次数据.
  Future<void> refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      _spots = await _apiService.getSpots(realOnly: _realOnly);
    } catch (_) {
      // 拉取失败保留上次数据
    }
    _isLoading = false;
    _refreshing = false;
    notifyListeners();
  }

  /* ==================== 模式 & 告警动作 ==================== */

  /// 切换 真实模式(仅真实) / 本地模式(真实+模拟), 全局生效并持久化.
  Future<void> toggleRealMode() async {
    _realOnly = !_realOnly;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_modeKey, _realOnly);
    notifyListeners();
    await refresh();
  }

  void ignoreAlert(AlertModel alert) {
    _ignoredAlertIds.add(alert.spotId);
    notifyListeners();
  }

  void resolveAlert(AlertModel alert) {
    _resolvedAlertIds.add(alert.spotId);
    notifyListeners();
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    _apiService.dispose();
    super.dispose();
  }
}
