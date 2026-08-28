import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';
import '../models/operation_log_model.dart';
import '../models/policy_config.dart';
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
  static const _layoutKey = 'spots_layout_mode';

  final ApiService _apiService;
  Timer? _refreshTimer;
  bool _refreshing = false;
  bool _gatewayOnline = false;  /* ⭐ 网关在线状态 */
  String? _policyError;         /* ⭐ 最近一次策略下发失败原因 */
  bool _policyPartial = false;  /* ⭐ 上次下发是否部分成功 (UI 琥珀色提示) */

  List<SpotModel> _spots = [];
  bool _realOnly = true; // true=真实模式(仅真实设备), 需模拟车位再切换本地模式
  bool _isLoading = true;
  int _layoutMode = 0; // 0=列表, 1=网格, 2=流式

  /* 策略配置: 集中管理告警/传感器/刷新/OTA 判定规则, 修改即时生效并持久化 */
  PolicyConfig _policy = const PolicyConfig();

  PolicyConfig get policy => _policy;
  int get alertSec => _policy.alertSec; // 停车超时告警阈值(秒)
  int get zombieThresholdSec => _policy.zombieThresholdSec; // ⭐ 僵尸车判定阈值(秒)
  int get sensorDistanceCm => _policy.sensorDistanceCm; // 传感器矛盾判定距离阈值(cm)
  int get refreshSec => _policy.refreshSec; // 数据刷新间隔(秒)
  bool get otaEnabled => _policy.otaEnabled; // OTA 自动检测开关
  int get otaIntervalSec => _policy.otaIntervalSec; // OTA 检测间隔(秒)
  int get otaCheckCount => _policy.otaCheckCount; // OTA 每轮检测次数
  bool get gatewayOnline => _gatewayOnline;  /* ⭐ 网关是否在线 */
  String? get policyError => _policyError;   /* ⭐ 策略下发失败原因 */
  bool get policyPartial => _policyPartial;  /* ⭐ 上次下发部分成功 */

  /* 告警处理记录 (按车位 spotId): 已处理由"僵尸车离开车位"自动派生 */
  /// 已自动处理的僵尸车快照 (spotId -> 告警): 车辆离开车位后自动标记为已处理, 供告警中心查看历史.
  final Map<String, AlertModel> _resolvedAlerts = {};

  /* 本地模拟: 通知/派单/处理 状态与处理记录 (仅内存, 3s 刷新后回写到 spots, 重启即丢) */
  final Set<String> _notifiedSpotIds = {};
  final Set<String> _dispatchedSpotIds = {};
  final Map<String, String> _handlerNames = {}; // spotId -> 处理人
  final Map<String, DateTime> _handledAts = {}; // spotId -> 完成时间

  /* 模拟车位本地状态切换覆盖 (spotId -> 状态/占用时长, 内存态, 3s 刷新后回写) */
  final Map<String, String> _mockStatusOverrides = {};
  final Map<String, int> _mockOccupiedOverrides = {};

  /* 批量选中集合 (收进 Provider, 跨页同步) */
  final Set<String> _selectedSpotIds = {};
  final Set<String> _selectedAlertIds = {};

  /* 操作日志 (内存态, 记录用户关键操作, 重启即清空) */
  final List<OperationLog> _operationLogs = [];

  /// 操作日志 (倒序: 最新在前).
  List<OperationLog> get operationLogs => List.unmodifiable(_operationLogs);

  /// 追加一条操作日志并通知刷新.
  void _addLog({
    required String type,
    required String title,
    String? spotId,
    String detail = '',
    bool success = true,
  }) {
    _operationLogs.insert(
      0,
      OperationLog(
        id: 'log_${DateTime.now().microsecondsSinceEpoch}',
        type: type,
        title: title,
        spotId: spotId,
        detail: detail,
        success: success,
        createdAt: DateTime.now(),
      ),
    );
    notifyListeners();
  }

  /* 小时级快照: 记录今天每小时的占用率 (0-100), 用于数据统计页折线图 */
  static const _hourlySnapshotKey = 'hourly_occupancy_snapshot';
  List<int> _hourlyOccupancy = List.filled(24, 0);
  int _lastSnapshotHour = -1;

  /// 启动流程: 先加载持久化的模式与策略, 再首次拉取, 最后启动全局轮询.
  Future<void> _init() async {
    await _loadMode();
    await _loadHourlySnapshot();
    await refresh();
    _restartRefreshTimer();
    // 固件升级为低频操作: 不常驻轮询, 由 MainShell 在每次进入前台时触发
    // 一轮短检测(beginOtaCheckSession), 检测到待升级任务时弹窗提示
  }

  Future<void> _loadMode() async {
    final prefs = await SharedPreferences.getInstance();
    _realOnly = prefs.getBool(_modeKey) ?? true;  /* 默认真实模式(仅真实设备) */
    _layoutMode = prefs.getInt(_layoutKey) ?? 0;
    _policy = await PolicyConfig.load();
  }

  /// 按当前刷新策略重建全局轮询定时器 (刷新间隔变化时立即生效).
  void _restartRefreshTimer() {
    _refreshTimer?.cancel();
    _refreshTimer =
        Timer.periodic(Duration(seconds: _policy.refreshSec), (_) => refresh());
  }

  /* ==================== 只读数据 ==================== */

  List<SpotModel> get spots => List.unmodifiable(_spots);
  bool get realOnly => _realOnly;
  bool get isLoading => _isLoading;
  int get layoutMode => _layoutMode;

  Future<void> setLayoutMode(int mode) async {
    _layoutMode = mode;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt(_layoutKey, mode);
    notifyListeners();
  }

  /// 更新策略配置: 需要下发的节点策略(僵尸车阈值)走 OneNET 同步服务调用
  /// (SetZombieThreshold, 定义在节点产品物模型上), 全部成功才提交持久化.
  /// 返回值: true=提交成功(全部成功/部分成功), false=网关离线或全部下发失败 (不提交, UI 回退).
  /// 部分成功时 [policyPartial]=true, 汇总原因在 [policyError], UI 用琥珀色提示.
  Future<bool> updatePolicy(PolicyConfig config) async {
    _policyPartial = false;  /* 每次下发重置部分成功标志 */
    final needDispatchZombie = config.zombieThresholdSec != _policy.zombieThresholdSec;

    /* ⭐ 需要网关下发的策略: 先查网关在线, 再逐节点同步服务调用 */
    if (needDispatchZombie) {
      final gatewayOnline = await _apiService.isGatewayOnline();
      if (!gatewayOnline) {
        _gatewayOnline = false;
        _policyError = '网关离线';
        notifyListeners();
        debugPrint('⚠️ 网关离线, 无法下发节点策略');
        return false;
      }

      /* 同步服务调用: 遍历所有【真实且未停用】节点下发 (模拟车位不参与平台下发,
       * 平台停用的设备也不再下发, 不参与成功/失败计数) */
      int successCount = 0;
      final failedSpots = <String>[];
      for (final spot in _spots.where((s) => s.isReal && !s.isDisabledSpot)) {
        final output = await _apiService.callService(
          spot.id,  // 节点设备名 (如 Park001, park2)
          'SetZombieThreshold',
          {'ThresholdValue': config.zombieThresholdSec},
          productId: ApiService.nodeProductId,  // 节点产品ID
        );
        final ok = output != null && output['Result'] == 1;
        debugPrint('${ok ? "✅" : "❌"} 节点 ${spot.id} 阈值下发: $output');
        if (ok) {
          successCount++;
        } else {
          failedSpots.add(spot.id);
        }
      }

      if (failedSpots.isNotEmpty && successCount == 0) {
        /* 全部失败: 不提交, UI 回退保持原值 */
        _policyError = '下发失败（${failedSpots.join('、')}），成功 0 个';
        notifyListeners();
        return false;
      }
      if (failedSpots.isNotEmpty) {
        /* 部分成功: 保存策略, 离线节点由网关在上线后自动补发 */
        _policyPartial = true;
        _policyError = '成功 $successCount 个，失败 ${failedSpots.length} 个'
            '（${failedSpots.join('、')}），离线节点上线后自动补发';
      }
    }

    _policy = config;
    await _policy.save();
    _restartRefreshTimer();
    if (!_policyPartial) _policyError = null;  /* 部分成功时保留汇总提示 */
    notifyListeners();
    return true;  /* 成功 */
  }

  int get totalSpots => _spots.length;
  List<SpotModel> get freeSpots => _spots.where((s) => s.isFree).toList();
  List<SpotModel> get occupiedSpots => _spots.where((s) => s.isOccupied).toList();
  List<SpotModel> get zombieSpots => _spots.where((s) => s.isZombie).toList();
  int get freeCount => freeSpots.length;
  int get occupiedCount => _spots.where((s) => s.status == 'occupied').length;
  int get zombieCount => zombieSpots.length;

  /// 全部告警 (含已自动处理的历史), 供告警中心状态筛选使用.
  List<AlertModel> get allAlerts {
    final list = <AlertModel>[];
    for (final spot in _spots) {
      // 告警只在车位成为僵尸车(占用≥僵尸阈值)时产生; 普通占用不生成告警
      final hasAlert = spot.isZombie;
      if (!hasAlert) continue;
      list.add(_buildAlert(spot));
    }
    // 追加已自动处理的僵尸车历史 (车辆已离开车位)
    list.addAll(_resolvedAlerts.values);
    list.sort((a, b) => b.occupiedHours.compareTo(a.occupiedHours));
    return list;
  }

  /// 活动告警 (排除已处理), 供概览等推送场景使用.
  /// 已处理的僵尸车不再推送, 但仍保留在 allAlerts 中供告警中心筛选查看历史.
  List<AlertModel> get alerts => allAlerts
      .where((a) => a.status != 'resolved')
      .toList();

  /// 由当前占用车位派生告警: pending=未通知 / notified=已通知 / dispatched=处理中.
  AlertModel _buildAlert(SpotModel spot) {
    final status = _dispatchedSpotIds.contains(spot.id)
        ? 'dispatched'
        : _notifiedSpotIds.contains(spot.id)
            ? 'notified'
            : 'pending';
    return AlertModel(
      id: 'alert_${spot.id}',
      plateNumber: spot.plateNumber ?? '未知车牌',
      spotId: spot.id,
      occupiedHours: spot.occupiedHours,
      status: status,
      createdAt: DateTime.now().subtract(Duration(hours: spot.occupiedHours)),
    );
  }

  /// 统计模型: 占用率/僵尸车数由 spots 派生, 一周趋势为演示数据.
  StatsModel get stats {
    final rate = totalSpots > 0 ? occupiedCount / totalSpots : 0.0;
    return StatsModel(
      totalSpots: totalSpots,
      occupiedSpots: occupiedCount,
      zombieSpots: zombieCount,
      occupancyRate: rate,
      hourlyTrend: _buildHourlyTrend(),
    );
  }

  List<HourlyTrend> _buildHourlyTrend() {
    final now = DateTime.now();
    final currentHour = now.hour;
    final rate = totalSpots > 0 ? (occupiedCount * 100 / totalSpots).round() : 0;
    _hourlyOccupancy[currentHour] = rate;
    return List.generate(24, (h) => HourlyTrend(
      hour: h,
      occupancyRate: _hourlyOccupancy[h],
    ));
  }

  Future<void> _loadHourlySnapshot() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_hourlySnapshotKey);
    if (raw != null) {
      try {
        final list = raw.split(',').map((e) => int.tryParse(e) ?? 0).toList();
        if (list.length == 24) {
          _hourlyOccupancy = list;
        }
      } catch (_) {}
    }
  }

  Future<void> _saveHourlySnapshot() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_hourlySnapshotKey, _hourlyOccupancy.join(','));
  }

  /* ==================== 数据刷新 ==================== */

  /// 唯一的数据拉取入口: 从 OneNET 拉取 (按当前模式过滤), 失败保留上次数据.
  /// 仅当数据真正变化时才 notifyListeners, 避免 3s 定时无条件触发全 APP 重建
  /// (每次重建都会重新绘制阴影/圆角/图标 → CanvasKit 着色器反复编译 → 卡顿/刷屏).
  Future<void> refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    bool changed = false;
    try {
      final prevById = {for (final s in _spots) s.id: s};
      final prevGatewayOnline = _gatewayOnline;
      _spots = await _apiService.getSpots(realOnly: _realOnly);
      _syncLocalState(); // 先回写本地模拟状态(含手动切换), 让自动处理看到一致的当前状态
      _handleAutoResolve(prevById); // 检测"僵尸车离开车位"→自动标记已处理
      
      /* ⭐ 同时检查网关在线状态 */
      _gatewayOnline = await _apiService.isGatewayOnline();
      changed = _gatewayOnline != prevGatewayOnline || _spotsChanged(prevById);
    } catch (_) {
      // 拉取失败保留上次数据
    }
    final prevLoading = _isLoading;
    _isLoading = false;
    _refreshing = false;
    if (_recordHourlySnapshot()) changed = true; // 整点快照记录也算一次数据变化
    if (prevLoading || changed) notifyListeners();
  }

  /// 对比本轮刷新前后车位数据, 判断是否真的有内容变化 (避免无谓重建).
  bool _spotsChanged(Map<String, SpotModel> prevById) {
    if (prevById.length != _spots.length) return true;
    for (final s in _spots) {
      final prev = prevById[s.id];
      if (prev == null) return true;
      if (prev.status != s.status ||
          prev.occupiedHours != s.occupiedHours ||
          prev.notifyStatus != s.notifyStatus ||
          prev.handlerName != s.handlerName ||
          prev.plateNumber != s.plateNumber) {
        return true;
      }
    }
    return false;
  }

  /// 记录每小时占用率快照; 返回是否产生了新快照 (供 refresh 判断是否需要通知).
  bool _recordHourlySnapshot() {
    final now = DateTime.now();
    final currentHour = now.hour;
    if (currentHour != _lastSnapshotHour) {
      _lastSnapshotHour = currentHour;
      final rate = totalSpots > 0 ? (occupiedCount * 100 / totalSpots).round() : 0;
      _hourlyOccupancy[currentHour] = rate;
      _saveHourlySnapshot();
      return true;
    }
    return false;
  }

  /* ==================== 模式 & 告警动作 ==================== */

  /// 切换 真实模式(仅真实) / 本地模式(真实+模拟), 全局生效并持久化.
  Future<void> toggleRealMode() async {
    _realOnly = !_realOnly;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_modeKey, _realOnly);
    _addLog(
      type: 'mode',
      title: '模式切换',
      detail: _realOnly ? '切换为真实模式（仅真实设备）' : '切换为本地模式（真实 + 模拟）',
    );
    notifyListeners();
    await refresh();
  }

  /***** 模拟车位本地状态切换 (仅内存态, 不落库, 用于本地演示) *****/
  /// 点击模拟车位图标循环切换其显示状态: 空闲→占用→僵尸车→空闲.
  /// 仅对模拟车位(isReal==false)生效; 真实/停用设备不参与.
  void cycleMockStatus(String id) {
    final idx = _spots.indexWhere((s) => s.id == id && !s.isReal);
    if (idx < 0) return;
    final spot = _spots[idx];
    String next;
    int occ;
    switch (spot.status) {
      case 'free':
        next = 'occupied';
        occ = 1;
        break;
      case 'occupied':
        next = 'zombie';
        occ = 2;
        break;
      default:
        next = 'free';
        occ = 0;
    }
    _spots[idx] = spot.copyWith(status: next, occupiedHours: occ);
    /* 记录覆盖值, 供 3s 刷新后回写, 防止模拟车位状态被刷新打回默认 */
    _mockStatusOverrides[id] = next;
    _mockOccupiedOverrides[id] = occ;
    notifyListeners();
  }

  /* ==================== 处理动作 (仅本地模拟 stub) ==================== */

  /// 通知车主: 调模拟 stub + 本地标记"已通知".
  Future<void> notifyOwner(SpotModel spot) async {
    await _apiService.notifyOwner(spot.id);
    _notifiedSpotIds.add(spot.id);
    _syncLocalState();
    _addLog(
      type: 'notify',
      title: '通知车主',
      spotId: spot.id,
      detail: '已通知 ${spot.plateNumber ?? spot.id} 车主尽快挪车',
    );
    notifyListeners();
  }

  /// 派单: 调模拟 stub + 设置处理人 + 标记"处理中".
  Future<void> dispatchSpot(SpotModel spot, {required String handlerName}) async {
    await _apiService.dispatchAlert(spot.id);
    _dispatchedSpotIds.add(spot.id);
    _handlerNames[spot.id] = handlerName;
    _handledAts.remove(spot.id);
    _syncLocalState();
    _addLog(
      type: 'dispatch',
      title: '派单处理',
      spotId: spot.id,
      detail: '派单给 $handlerName 现场处理',
    );
    notifyListeners();
  }

  /// 批量通知: 并行调 stub + 本地标记"已通知" (避免逐台串行等待放大延迟).
  Future<void> notifySpots(Iterable<SpotModel> spots) async {
    final list = spots.toList();
    await Future.wait(list.map((s) => _apiService.notifyOwner(s.id)));
    _notifiedSpotIds.addAll(list.map((s) => s.id));
    _syncLocalState();
    _addLog(
      type: 'notify',
      title: '批量通知车主',
      detail: '已通知 ${list.length} 个车位的车主挪车',
    );
    notifyListeners();
  }

  /// 批量派单: 并行调 stub + 默认处理人 + 标记"处理中" (避免逐台串行等待放大延迟).
  Future<void> dispatchSpots(Iterable<SpotModel> spots) async {
    final list = spots.toList();
    await Future.wait(list.map((s) => _apiService.dispatchAlert(s.id)));
    for (final s in list) {
      _dispatchedSpotIds.add(s.id);
      _handlerNames.putIfAbsent(s.id, () => '张师傅');
      _handledAts.remove(s.id);
    }
    _syncLocalState();
    _addLog(
      type: 'dispatch',
      title: '批量派单',
      detail: '已派单 ${list.length} 个车位',
    );
    notifyListeners();
  }

  /* ==================== 批量选中 (收进 Provider 跨页同步) ==================== */

  Set<String> get selectedSpotIds => Set.unmodifiable(_selectedSpotIds);
  Set<String> get selectedAlertIds => Set.unmodifiable(_selectedAlertIds);
  bool isSpotDispatched(String spotId) => _dispatchedSpotIds.contains(spotId);

  /// 车位当前告警处理状态: pending=未通知 / notified=已通知 / dispatched=处理中 / resolved=已处理.
  String spotAlertStatus(String spotId) {
    if (_resolvedAlerts.containsKey(spotId)) return 'resolved';
    if (_dispatchedSpotIds.contains(spotId)) return 'dispatched';
    if (_notifiedSpotIds.contains(spotId)) return 'notified';
    return 'pending';
  }

  void toggleSpotSelection(String spotId) {
    if (!_selectedSpotIds.add(spotId)) _selectedSpotIds.remove(spotId);
    notifyListeners();
  }

  void toggleAlertSelection(String alertId) {
    if (!_selectedAlertIds.add(alertId)) _selectedAlertIds.remove(alertId);
    notifyListeners();
  }

  void selectAllSpots(Iterable<SpotModel> spots) {
    _selectedSpotIds
      ..clear()
      ..addAll(spots.map((s) => s.id));
    notifyListeners();
  }

  void selectAllAlerts(Iterable<AlertModel> alerts) {
    _selectedAlertIds
      ..clear()
      ..addAll(alerts.map((a) => a.id));
    notifyListeners();
  }

  void clearSpotSelection() {
    _selectedSpotIds.clear();
    notifyListeners();
  }

  void clearAlertSelection() {
    _selectedAlertIds.clear();
    notifyListeners();
  }

  /// 把本地模拟状态回写到当前 spots, 保证 3s 刷新后新实例仍保持 (内存态, 重启即丢).
  /// status/occupiedHours 为不可变字段, 用 copyWith 重建; 其余可变字段直接赋值.
  void _syncLocalState() {
    _spots = [
      for (final spot in _spots) _applyLocalStateToSpot(spot),
    ];
  }

  SpotModel _applyLocalStateToSpot(SpotModel spot) {
    var s = spot;
    /* 模拟车位: 回写用户手动切换的状态, 防止 3s 刷新打回默认 */
    if (!s.isReal) {
      final status = _mockStatusOverrides[s.id];
      if (status != null) {
        s = s.copyWith(
          status: status,
          occupiedHours: _mockOccupiedOverrides[s.id] ?? s.occupiedHours,
        );
      }
    }
    if (_notifiedSpotIds.contains(s.id)) {
      s.notifyStatus = 'notified';
    }
    final name = _handlerNames[s.id];
    if (name != null) {
      s.handlerName = name;
    }
    final handledAt = _handledAts[s.id];
    if (handledAt != null) {
      s.handledAt = handledAt;
    }
    return s;
  }

  /// 检测车位状态转换并自动处理 (每次刷新后调用):
  /// - 僵尸车/超时占用车离开车位 → 自动标记为"已处理" (记录快照供告警中心查看历史)
  /// - 新车进场开始占用 → 开启新一轮, 清空上一轮的 已处理/忽略/通知/派单 记录
  void _handleAutoResolve(Map<String, SpotModel> prevById) {
    bool wasAlert(SpotModel s) => s.isZombie; // 告警仅在成为僵尸车时产生

    for (final spot in _spots) {
      final prev = prevById[spot.id];
      final prevHadAlert = prev != null && wasAlert(prev);
      final nowHasAlert = wasAlert(spot);

      if (prevHadAlert && !nowHasAlert) {
        // 车辆已离开车位 → 自动处理为"已处理"
        _resolvedAlerts[spot.id] = AlertModel(
          id: 'alert_${spot.id}',
          plateNumber: prev.plateNumber ?? '未知车牌',
          spotId: spot.id,
          occupiedHours: prev.occupiedHours,
          status: 'resolved',
          createdAt: DateTime.now().subtract(Duration(hours: prev.occupiedHours)),
        );
        _handledAts[spot.id] = DateTime.now();
        _handlerNames.putIfAbsent(spot.id, () => '自动处理');
        _dispatchedSpotIds.remove(spot.id);
        _notifiedSpotIds.remove(spot.id);
      } else if (!prevHadAlert && nowHasAlert) {
        // 新车进场开始占用 → 上一轮记录作废, 重新从待处理开始
        _resolvedAlerts.remove(spot.id);
        _dispatchedSpotIds.remove(spot.id);
        _notifiedSpotIds.remove(spot.id);
        _handlerNames.remove(spot.id);
        _handledAts.remove(spot.id);
      }
    }
  }

  /* ==================== 固件升级 (OTA) ====================
   * 全网一键升级: App 查任务/查状态(经 fuse-ota sha1 签名), 下发 OtaAllow 确认门控.
   * 方法转发给 ApiService, 保持"所有平台调用收敛在 Provider"的架构约束. */

  Future<String?> getOtaNodeVersion(String deviceName) =>
      _apiService.getOtaNodeVersion(deviceName);

  Future<Map<String, dynamic>?> getOtaTask(String deviceName, {required String version}) =>
      _apiService.getOtaTask(deviceName, version: version);

  /// 查询指定升级任务状态 (官方 fuse-ota $tid/check), 驱动阶段文本与完成/失败检测.
  Future<int?> getOtaTaskStatus(String tid) =>
      _apiService.getOtaTaskStatus(_otaNodeDevice, tid);

  /// 下发全网升级确认/复位 (OtaAllow 布尔属性: true=确认 false=复位).
  /// OtaAllow 已迁到网关侧: 目标是网关设备 PGW001(网关产品), 不再是节点.
  Future<bool> setOtaAllow(bool allow) =>
      _apiService.setProperty(ApiService.gatewayDeviceId, 'OtaAllow', allow,
          productId: ApiService.gatewayProductId);

  /* ---- OTA 升级自动检测与弹窗 ---- */

  static const String _otaNodeDevice = 'Park001';
  static const String _otaDefaultVersion = 'v2.321'; // 读不到版本时的兜底(NODE_FW_VERSION)

  Timer? _otaCheckTimer;
  Timer? _otaPollTimer;
  int _otaChecksLeft = 0; // 本轮短检测剩余次数
  final Set<String> _otaIgnoredTids = {}; // 本次会话用户点过"忽略"的任务
  String? _otaCurrentVersion;
  Map<String, dynamic>? _otaTask;
  String? _otaTid;
  String? _otaTarget;
  int? _otaStatus; // 1待升级 2下载中 3升级中 4成功 5失败 6取消 (官方 fuse-ota $tid/check)
  int _otaProgress = 0; // 真实进度0-100: 下载阶段0, LoRa分发阶段为已发送字节占比
  bool _otaConfirming = false;
  bool _otaPromptVisible = false;
  bool _otaDoneHold = false; // 完成时先展示100%进度1秒再收起, 让用户看清

  String? get otaCurrentVersion => _otaCurrentVersion;
  String? get otaTarget => _otaTarget;
  int? get otaStatus => _otaStatus;
  int get otaStep => _otaProgress;
  bool get otaConfirming => _otaConfirming;
  bool get otaPromptVisible => _otaPromptVisible;
  /// 显示进度条的条件: 升级中(已确认下发 OtaAllow) 或 完成后的1秒停留(让用户看清100%).
  /// 注意: 未点击"立即升级"的待升级(status=1)不显示进度条.
  bool get otaShowProgress {
    return _otaConfirming || _otaDoneHold;
  }

  /// 每次进入前台(或App启动)触发一轮 OTA 检测:
  /// 立即查 1 次 + 每 [otaIntervalSec]s 再查 [otaCheckCount]-1 次(共 [otaCheckCount] 次),
  /// 之后自动停止. 检测间隔/次数/开关来自策略配置, 修改即时生效.
  /// 固件升级是低频操作, 无需常驻轮询, 避免长期占用 OneNET 请求配额.
  void beginOtaCheckSession() {
    if (_otaConfirming) return; // 升级进行中不重启检测会话, 避免再次触发升级弹窗
    if (!_policy.otaEnabled) return; // 关闭 OTA 自动检测: 不启动检测会话
    _otaCheckTimer?.cancel();
    _otaChecksLeft = _policy.otaCheckCount - 1;
    _checkOtaTask(); // 进入前台立即查一次
    _otaCheckTimer = Timer.periodic(
        Duration(seconds: _policy.otaIntervalSec), (_) {
      if (_otaChecksLeft <= 0) {
        _otaCheckTimer?.cancel();
        return;
      }
      _otaChecksLeft--;
      _checkOtaTask();
    });
  }

  /// 周期检测平台是否有待升级任务; 有待升级任务(1)且未被忽略 → 置位弹窗.
  Future<void> _checkOtaTask() async {
    if (_otaConfirming) return; // 升级进行中不重复检测
    final version =
        await _apiService.getOtaNodeVersion(_otaNodeDevice) ?? _otaDefaultVersion;
    final task = await _apiService.getOtaTask(_otaNodeDevice, version: version);
    // 网络请求期间用户可能已在固件升级页点击"确认升级": 升级已在进行中,
    // 直接终止本轮检测, 避免把检测结果再次置为"待升级"而触发全局升级弹窗.
    if (_otaConfirming) return;
    _otaCurrentVersion = version;

    if (task == null) {
      final had = _otaTask != null || _otaPromptVisible;
      _otaTask = null;
      _otaTid = null;
      _otaTarget = null;
      _otaStatus = null;
      _otaPromptVisible = false;
      if (had) notifyListeners();
      return;
    }

    final tid = task['tid']?.toString();
    final status = task['status'] as int?;
    _otaTask = task;
    _otaTid = tid;
    _otaTarget = task['target']?.toString();
    _otaStatus = status;
    if (tid != null &&
        status == 1 &&
        !_otaIgnoredTids.contains(tid) &&
        !_otaPromptVisible) {
      _otaPromptVisible = true;
    }
    notifyListeners();
  }

  /// 固件升级页进入时同步查询到的任务到全局状态机(仅当没有进行中的任务),
  /// 让固件升级页与升级弹窗共用同一份任务状态/进度显示.
  void syncOtaTask(Map<String, dynamic>? task, String version) {
    if (_otaConfirming) return; // 已有进行中任务(弹窗发起), 页面跟随全局状态即可
    _otaDoneHold = false; // 页面重新同步: 收起上一次的完成停留
    _otaCurrentVersion = version;
    final tid = task?['tid']?.toString();
    final changed = tid != _otaTid ||
        _otaTarget != task?['target']?.toString() ||
        _otaStatus != (task?['status'] as int?);
    _otaTid = tid;
    _otaTarget = task?['target']?.toString();
    _otaStatus = task?['status'] as int?;
    if (changed) notifyListeners();
  }

  /// 固件升级页手动升级入口: 与升级弹窗共用同一套 OTA 状态机/模拟进度.
  /// 传入任务 tid/target, 走统一 confirm 流程(下发 OtaAllow + 轮询 + 完成复位).
  Future<bool> startManualOta(String tid, String target) async {
    if (_otaConfirming) return true;
    _otaTid = tid;
    _otaTarget = target;
    _otaStatus = 1;
    _otaPromptVisible = false;
    notifyListeners();
    return confirmOtaUpgrade();
  }

  /// 弹窗点击"立即升级": 下发 OtaAllow=true 确认, 网关收到后执行, 轮询任务状态.
  /// 返回确认指令是否下发成功; 成功后保持轮询, 完成后自动复位 OtaAllow=false.
  Future<bool> confirmOtaUpgrade() async {
    if (_otaConfirming) return true;
    _otaConfirming = true;
    _otaPromptVisible = false;
    _otaDoneHold = false; // 新一轮升级: 收起上一次的完成停留
    _otaCheckTimer?.cancel(); // 已确认升级: 终止本轮任务检测, 不再继续轮询
    notifyListeners();

    final ok = await setOtaAllow(true);
    if (!ok) {
      _otaConfirming = false;
      notifyListeners();
      return false;
    }
    _addLog(
      type: 'ota',
      title: '固件升级',
      spotId: _otaNodeDevice,
      detail: '确认升级至 ${_otaTarget ?? '未知版本'}',
    );
    _otaProgress = 0; // 重新升级: 真实进度清零, 由网关上报驱动
    _otaPollTimer?.cancel();
    _pollOtaStatus();
    _otaPollTimer =
        Timer.periodic(const Duration(seconds: 5), (_) => _pollOtaStatus());
    return true;
  }

  /// 轮询 OTA 状态/进度: 阶段状态用官方 fuse-ota $tid/check(任务独立, 完成/失败
  /// 自动落为4/5, 天然避免"下次升级误读旧状态"), 精确进度条用网关 OtaProgress
  /// (0-100, 下载阶段0, LoRa 分发阶段为网关实际发送字节占比). 完成/失败后停止
  /// 轮询并复位 OtaAllow=false; 任一查询失败时保留上一轮值等下一轮.
  Future<void> _pollOtaStatus() async {
    // 1. 官方任务状态: 驱动阶段文本与完成/失败检测
    final tid = _otaTid;
    if (tid != null) {
      final status = await getOtaTaskStatus(tid);
      if (status != null) _otaStatus = status;
    }
    // 2. 网关 OtaProgress: 驱动精确进度条 (阶段状态不再依赖网关属性)
    final state = await _apiService.getGatewayOtaState();
    if (state != null) {
      final progress = state['OtaProgress'];
      if (progress is int) _otaProgress = progress.clamp(0, 100);
    }

    if (_otaStatus != null && _otaStatus! >= 4) {
      // 终端状态: 停止轮询并复位 OtaAllow 确认门控
      _otaPollTimer?.cancel();
      await setOtaAllow(false);
      _otaConfirming = false;
      if (_otaStatus == 4) {
        // 完成: 进度置满, 停留1秒展示100%后再收起进度条
        _otaProgress = 100;
        _otaDoneHold = true;
        Timer(const Duration(seconds: 1), () {
          _otaDoneHold = false;
          notifyListeners();
        });
      }
    }
    notifyListeners();
  }

  /// 关闭升级弹窗; ignore=true 表示用户点"忽略", 本次会话不再弹该任务.
  /// 无论何种操作(忽略/关闭/兜底)都终止本轮任务检测, 不再继续轮询.
  void dismissOtaPrompt({bool ignore = false}) {
    _otaCheckTimer?.cancel(); // 弹窗被关闭: 直接终止检测轮询
    if (ignore && _otaTid != null) _otaIgnoredTids.add(_otaTid!);
    _otaPromptVisible = false;
    notifyListeners();
  }

  /// 阶段文本推断: 终端状态用官方 status(4/5/6), 中间档位结合官方 status 与
  /// OtaProgress 推断 —— 官方 $tid/check 在执行期可能恒为 1(实测不返回实时 step),
  /// 故确认后以 OtaProgress>0 判定进入"升级中"(LoRa 分发), 否则为"固件下载中".
  String get otaStatusText {
    switch (_otaStatus) {
      case 4: return '升级完成';
      case 5: return '升级失败';
      case 6: return '已取消';
      case 2:
        // 官方 status=2(下载中/升级中, 平台将 0<step<100 映射为2)
        return _otaProgress > 0 ? '升级中…' : '固件下载中…';
      case 3:
        return '升级中…';
      case 1:
      default:
        if (_otaConfirming) {
          return _otaProgress > 0 ? '升级中…' : '固件下载中…';
        }
        return '待升级';
    }
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    _otaCheckTimer?.cancel();
    _otaPollTimer?.cancel();
    _apiService.dispose();
    super.dispose();
  }
}
