import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';
import '../models/operation_log_model.dart';
import '../models/policy_config.dart';
import '../services/api_service.dart';

/// ⭐ 智能格式化等待秒数: 自动选择最紧凑的单位, 避免 "0小时" 这种无意义显示
/// 规则:
///   < 60s  → "45秒"
///   < 3600s → "15分30秒"
///   < 86400s → "2时30分" (秒被吞掉, 因为等待时长通常 >= 分钟级)
///   >= 86400s → "1天2时"
String _formatWaitSec(int sec) {
  if (sec < 60) return '$sec秒';
  if (sec < 3600) {
    final m = sec ~/ 60;
    final s = sec % 60;
    return s > 0 ? '$m分$s秒' : '$m分';
  }
  if (sec < 86400) {
    final h = sec ~/ 3600;
    final m = (sec % 3600) ~/ 60;
    return m > 0 ? '$h时$m分' : '$h时';
  }
  final d = sec ~/ 86400;
  final h = (sec % 86400) ~/ 3600;
  return h > 0 ? '$d天$h时' : '$d天';
}

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
  static const _notifiedAtPrefix = 'notified_at_'; // 🆕 通知时间戳 SharedPreferences 前缀 (key=alertId)
  static const _spotDispatchedPrefix = 'spot_dispatched_'; // ⭐⭐⭐ 按 spotId 存派单状态 (key=spotId, 避免 alertId 因 occupiedSince 反推不准导致匹配失败)

  final ApiService _apiService;
  Timer? _refreshTimer;  /* 3s API轮询定时器 */
  Timer? _tickTimer;     /* ⭐⭐⭐ 1s UI刷新定时器: 只要有占用车位, 每秒触发一次UI重建让秒数跳动 */
  bool _refreshing = false;
  DateTime? _lastRefreshAt;  /* ⭐ 上次成功刷新时间, 用于"3秒前"显示 */
  bool _gatewayOnline = false;  /* ⭐ 网关在线状态 */
  String? _policyError;         /* ⭐ 最近一次策略下发失败原因 */
  bool _policyPartial = false;  /* ⭐ 上次下发是否部分成功 (UI 琥珀色提示) */
  bool _nodeThresholdSynced = false; /* 🆕 是否已同步过节点真实阈值到本地(重启只同步1次, 避免3s反复改) */

  List<SpotModel> _spots = [];
  bool _realOnly = true; // true=真实模式(仅真实设备), 需模拟车位再切换本地模式
  bool _isLoading = true;
  int _layoutMode = 0; // 0=列表, 1=网格, 2=流式
  /* ⭐⭐ 模式切换性能优化: 完整拉取后缓存"全部车位(含模拟)", 切模式仅按 _realOnly 本地过滤, 0 延迟
   * 避免每次切模式都调 OneNET API, 网络慢→动画前卡顿, _refreshing 锁→偶发切失败 */
  List<SpotModel> _allSpotsCache = [];
  bool _allSpotsCacheFetched = false; // 是否已拉取过完整数据集
  bool _skipNextAutoRefresh = false; // 模式切换后跳过下一轮 3s 定时 refresh, 避免抢锁
  bool _modeSwitching = false; // 是否处于"切模式动画中", 防止 _tickTimer 触发 rebuild 导致动画撕裂

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

  /* ⭐⭐⭐ 告警事件隔离机制: 按「每次停车事件」生成独立工单, 不再和车位永久绑定
   * 同一车位发生 N 次僵尸车事件 → 生成 N 个独立告警（独立id、独立时间戳、独立处理流程）*/
  /// 当前每个车位对应的【正在处理的活跃工单】: spotId → AlertModel (同一个车位同时刻只有1个active)
  final Map<String, AlertModel> _activeAlerts = {};
  /// 已处理归档工单: key = alertId (事件级唯一), 不是spotId → 同一车位的历史事件都会被保留, 不会被新车覆盖
  final Map<String, AlertModel> _resolvedAlerts = {};

  /* 本地模拟: 通知/派单/处理 状态与处理记录 (KEY = alertId 事件级, 不是spotId车位级) */
  final Set<String> _notifiedAlertIds = {};   // 已发出通知的工单id
  final Set<String> _dispatchedAlertIds = {}; // 已派单的工单id
  final Map<String, String> _handlerNames = {};    // alertId -> 处理人
  final Map<String, DateTime> _handledAts = {};    // alertId -> 完成时间
  final Map<String, DateTime> _notifiedAts = {};   // alertId -> 通知车主时间（用于派单倒计时, 持久化）
  final Map<String, DateTime> _dispatchedAts = {}; // alertId -> 派单时间

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
    _restartTickTimer(); // ⭐⭐⭐ 启动1秒粒度秒数跳动定时器
    // 固件升级为低频操作: 不常驻轮询, 由 MainShell 在每次进入前台时触发
    // 一轮短检测(beginOtaCheckSession), 检测到待升级任务时弹窗提示
  }

  Future<void> _loadMode() async {
    final prefs = await SharedPreferences.getInstance();
    _realOnly = prefs.getBool(_modeKey) ?? true;  /* 默认真实模式(仅真实设备) */
    _layoutMode = prefs.getInt(_layoutKey) ?? 0;
    _policy = await PolicyConfig.load();
    // ⭐ 加载持久化的工单通知时间戳 (key=alertId)
    for (final key in prefs.getKeys()) {
      if (key.startsWith(_notifiedAtPrefix)) {
        final alertId = key.substring(_notifiedAtPrefix.length);
        final ts = prefs.getInt(key);
        if (ts != null) {
          _notifiedAts[alertId] = DateTime.fromMillisecondsSinceEpoch(ts);
          _notifiedAlertIds.add(alertId);
        }
      }
    }
    /* ⭐⭐⭐ 按 spotId 加载派单状态 (避免 alertId 因 occupiedSince 反推不准导致匹配失败)
     * 先暂存到临时 map, 等 refresh() 生成 active 工单后再按 spotId 灌进去 */
    for (final key in prefs.getKeys()) {
      if (key.startsWith(_spotDispatchedPrefix)) {
        final spotId = key.substring(_spotDispatchedPrefix.length);
        final ts = prefs.getInt(key);
        if (ts != null) _pendingDispatchedSpots[spotId] = DateTime.fromMillisecondsSinceEpoch(ts);
      }
    }
  }

  /// ⭐⭐⭐ APP 启动时暂存的"已派单 spotId → 派单时间", 等 refresh() 生成 active 工单后按 spotId 灌进 _dispatchedAlertIds
  final Map<String, DateTime> _pendingDispatchedSpots = {};

  /// 按当前刷新策略重建全局轮询定时器 (刷新间隔变化时立即生效).
  void _restartRefreshTimer() {
    _refreshTimer?.cancel();
    _refreshTimer =
        Timer.periodic(Duration(seconds: _policy.refreshSec), (_) {
      // ⭐ 模式切换后立即跳过下一轮 3s 定时 refresh, 避免和动画/后台refresh抢 _refreshing 锁
      if (_skipNextAutoRefresh) {
        _skipNextAutoRefresh = false;
        return;
      }
      refresh();
    });
  }

  /// ⭐⭐⭐ 1秒粒度【占用秒数跳动定时器】: 解决"3秒API刷新成功但秒数看起来不变"的用户感知问题
  /// - 仅当存在 占用/僵尸 车位时才每秒 notifyListeners() → 不浪费性能
  /// - 不调 refresh, 不请求API → 只是触发Consumer重新build, 调用 actualOccupiedSec getter 计算 now - occupiedSince, 秒数自动+1
  /// - 效果: 用户肉眼看到「5秒→6秒→7秒→8秒」每秒跳动, 不会再有"卡住不更新"的错觉
  void _restartTickTimer() {
    _tickTimer?.cancel();
    _tickTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      // ⭐ 模式切换动画期间暂停秒数跳动 rebuild, 避免动画撕裂(位移动画和notify同时跑)
      if (_modeSwitching) return;
      final hasOccupied = _spots.any((s) => s.isOccupied || s.isZombie);
      if (!hasOccupied) return;
      notifyListeners();
    });
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

  /// 更新策略配置: 需要下发的节点策略(僵尸车阈值/超声波距离阈值)走 OneNET 同步
  /// 服务调用(SetZombieThreshold/SetSensorDistance, 定义在节点产品物模型上),
  /// 全部成功才提交持久化.
  /// 返回值: true=提交成功(全部成功/部分成功), false=网关离线或全部下发失败 (不提交, UI 回退).
  /// 部分成功时 [policyPartial]=true, 汇总原因在 [policyError], UI 用琥珀色提示.
  Future<bool> updatePolicy(PolicyConfig config) async {
    _policyPartial = false;  /* 每次下发重置部分成功标志 */
    final needDispatchZombie = config.zombieThresholdSec != _policy.zombieThresholdSec;
    final needDispatchSensor = config.sensorDistanceCm != _policy.sensorDistanceCm;

    /* ⭐ 乐观更新: 立即把新值显示到 UI, 下发全部失败才回退 */
    final oldPolicy = _policy;
    _policy = config;
    notifyListeners();

    /* ⭐ 需要网关下发的策略: 先查网关在线, 再逐节点同步服务调用
     * 僵尸车阈值与超声波距离阈值共享一次网关在线检查, 各自独立下发并汇总结果 */
    if (needDispatchZombie || needDispatchSensor) {
      final gatewayOnline = await _apiService.isGatewayOnline();
      if (!gatewayOnline) {
        _gatewayOnline = false;
        _policyError = '网关离线';
        /* ⭐ 全部失败(网关离线): 回退旧值 */
        _policy = oldPolicy;
        notifyListeners();
        debugPrint('⚠️ 网关离线, 无法下发节点策略');
        return false;
      }

      /* 真实且未停用的节点 (模拟车位/平台停用设备不参与平台下发与计数) */
      final realSpots = _spots.where((s) => s.isReal && !s.isDisabledSpot).toList();
      final errors = <String>[];
      int totalSuccess = 0;
      int totalFail = 0;

      /* ---- 僵尸车阈值下发 ---- */
      if (needDispatchZombie) {
        int successCount = 0;
        final failedSpots = <String>[];
        for (final spot in realSpots) {
          final output = await _apiService.callService(
            spot.id,
            'SetZombieThreshold',
            {'ThresholdValue': config.zombieThresholdSec},
            productId: ApiService.nodeProductId,
          );
          final ok = output != null && output['Result'] == 1;
          debugPrint('${ok ? "✅" : "❌"} 节点 ${spot.id} 僵尸车阈值下发: $output');
          if (ok) {
            successCount++;
          } else {
            failedSpots.add(spot.id);
          }
        }
        totalSuccess += successCount;
        totalFail += failedSpots.length;
        if (failedSpots.isNotEmpty && successCount > 0) {
          /* ⭐ 部分失败(有成功): 离线节点由网关自动补发 */
          errors.add('僵尸车阈值: 成功 $successCount 个，失败 ${failedSpots.length} 个'
              '（${failedSpots.join('、')}），离线节点上线后自动补发');
        } else if (failedSpots.isNotEmpty) {
          /* ⭐ 全部失败: 网关离线/平台异常, 补发无从谈起 */
          errors.add('僵尸车阈值下发全部失败');
        }
      }

      /* ---- ⭐ 超声波距离阈值下发 ---- */
      if (needDispatchSensor) {
        int successCount = 0;
        final failedSpots = <String>[];
        for (final spot in realSpots) {
          final output = await _apiService.callService(
            spot.id,
            'SetSensorDistance',
            {'DistanceValue': config.sensorDistanceCm},
            productId: ApiService.nodeProductId,
          );
          final ok = output != null && output['Result'] == 1;
          debugPrint('${ok ? "✅" : "❌"} 节点 ${spot.id} 超声波距离阈值下发(${config.sensorDistanceCm}cm): $output');
          if (ok) {
            successCount++;
          } else {
            failedSpots.add(spot.id);
          }
        }
        totalSuccess += successCount;
        totalFail += failedSpots.length;
        if (failedSpots.isNotEmpty && successCount > 0) {
          errors.add('超声波距离阈值: 成功 $successCount 个，失败 ${failedSpots.length} 个'
              '（${failedSpots.join('、')}），离线节点上线后自动补发');
        } else if (failedSpots.isNotEmpty) {
          errors.add('超声波距离阈值下发全部失败');
        }
      }

      /* 全部失败: 回退旧值 */
      if (totalFail > 0 && totalSuccess == 0) {
        _policy = oldPolicy;
        _policyError = errors.join('；');
        notifyListeners();
        return false;
      }
      /* 部分成功: 保存策略, 离线节点由网关在上线后自动补发 */
      if (totalFail > 0) {
        _policyPartial = true;
        _policyError = errors.join('；');
      }
    }

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
    // 追加已处理归档的历史工单 (事件级独立存储 → 不会被新事件覆盖, 同一车位多条历史都保留)
    list.addAll(_resolvedAlerts.values);
    // 按「工单创建时间」倒序（最新的排前面），不再用占用时长排序，因为新老工单时长不一样
    list.sort((a, b) => (b.createdAt ?? DateTime(2000)).compareTo(a.createdAt ?? DateTime(2000)));
    return list;
  }

  /// 活动告警 (排除已处理), 供概览等推送场景使用.
  /// 已处理的僵尸车不再推送, 但仍保留在 allAlerts 中供告警中心筛选查看历史.
  List<AlertModel> get alerts => allAlerts
      .where((a) => a.status != 'resolved')
      .toList();

  /// ⭐⭐⭐ 【按事件生成/复用工单】核心修复: 同一车位多次僵尸车事件 → 生成多个独立工单
  /// 规则:
  /// 1. 当前有_activeAlerts[spot.id]（说明同一个停车事件的工单还在处理中）→ 复用这个工单ID和createdAt,
  ///    只更新占用秒数/状态, 保证时间连续、处理记录不丢失
  /// 2. 当前没有active（新车第一次变僵尸车 / 或者上一个事件已经归档结束）→ 生成全新事件级唯一id的工单
  AlertModel _buildAlert(SpotModel spot) {
    final realOcc = spot.actualOccupiedSec;
    final active = _activeAlerts[spot.id];

    /* 情况1: 同一个事件正在处理中 → 直接复用id/createdAt, 更新状态/秒数 */
    if (active != null) {
      final status = _dispatchedAlertIds.contains(active.id)
          ? 'dispatched'
          : _notifiedAlertIds.contains(active.id)
              ? 'notified'
              : 'pending';
      final updated = active.copyWith(
        status: status,
        plateNumber: spot.plateNumber ?? active.plateNumber,
        occupiedSec: realOcc,
        occupiedHours: realOcc ~/ 3600,
      );
      _activeAlerts[spot.id] = updated; // 同步最新值到active缓存
      return updated;
    }

    /* 情况2: 新事件 → 生成全新工单id（事件级唯一，同一车位不同事件id不同）*/
    // 用「进场时间戳」做后缀，保证每次停车事件都是新id，不会和该车位的历史工单id冲突
    final eventTs = spot.occupiedSince?.millisecondsSinceEpoch ?? DateTime.now().millisecondsSinceEpoch;
    final alertId = 'alert_${spot.id}_$eventTs';
    final createdAt = realOcc > 0
        ? DateTime.now().subtract(Duration(seconds: realOcc))
        : DateTime.now();

    var newAlert = AlertModel(
      id: alertId,
      plateNumber: spot.plateNumber ?? '未知车牌',
      spotId: spot.id,
      occupiedHours: realOcc ~/ 3600,
      occupiedSec: realOcc,
      status: 'pending', /* 新事件默认未通知 */
      createdAt: createdAt,
    );
    _activeAlerts[spot.id] = newAlert; // 标记为当前活跃工单

    /* ⭐⭐⭐ APP 启动恢复: 如果这个 spotId 在持久化派单列表里, 灌进派单状态
     * （alertId 因 occupiedSince 反推可能变了, 但 spotId 不变, 按 spotId 匹配更可靠）*/
    final dispatchedAt = _pendingDispatchedSpots.remove(spot.id);
    if (dispatchedAt != null) {
      _dispatchedAlertIds.add(alertId);
      _dispatchedAts[alertId] = dispatchedAt;
      _notifiedAlertIds.remove(alertId);  // 派过单就不该再显示已通知
      _notifiedAts.remove(alertId);
      newAlert = newAlert.copyWith(status: 'dispatched');
      _activeAlerts[spot.id] = newAlert;
    }

    return newAlert;
  }

  /// ⭐⭐⭐ 统计模型: 比赛级看板数据, 全部从 spots + alerts 实时派生
  StatsModel get stats {
    final rate = totalSpots > 0 ? occupiedCount / totalSpots : 0.0;

    /* 在线设备数: 网关 + 在线节点
     * 真实模式: 只算真实在线节点; 模拟模式: 模拟车位视为在线(演示态), 与车位列表实时联动 */
    final gatewayOnline = _gatewayOnline || !_realOnly; // 模拟模式网关视为在线
    final onlineNodes = _spots.where((s) {
      if (s.isDisabledSpot) return false;
      if (s.isReal) return s.isOnline;
      return !_realOnly; // 模拟车位在本地模式下视为在线
    }).length;
    final onlineDevices = (gatewayOnline ? 1 : 0) + onlineNodes;

    /* 今日告警处理: 从 alerts 派生 */
    final today = DateTime.now();
    bool isToday(DateTime? dt) =>
        dt != null && dt.year == today.year && dt.month == today.month && dt.day == today.day;

    int notified = 0, dispatched = 0, resolved = 0;
    for (final alert in allAlerts) {
      final aid = alert.id;
      /* ⭐ 今日处理效率: 按处理动作时间戳判断今天(派单/通知/完成时间),
       * 不用告警创建时间 createdAt —— 否则跨天告警今天派单会被漏算, 与告警中心不一致 */
      final dAt = _dispatchedAts[aid];
      final nAt = _notifiedAts[aid];
      final hAt = _handledAts[aid];
      if (dAt != null && isToday(dAt)) {
        dispatched++;
      } else if (nAt != null && isToday(nAt)) {
        notified++;
      }
      if (hAt != null && isToday(hAt)) resolved++;
    }

    /* 平均占用时长(分钟): 算当前正在占用的车位(真实+模拟), 模拟模式与车位列表联动 */
    int totalOccMin = 0;
    int occCount = 0;
    for (final s in _spots) {
      if (!s.isDisabledSpot && (s.isOccupied || s.isZombie)) {
        totalOccMin += s.actualOccupiedSec ~/ 60;
        occCount++;
      }
    }
    final avgOccMin = occCount > 0 ? totalOccMin ~/ occCount : 0;

    /* 上次刷新距今 */
    String lastRefreshAgo = '-';
    if (_lastRefreshAt != null) {
      final diff = DateTime.now().difference(_lastRefreshAt!);
      if (diff.inSeconds < 5) lastRefreshAgo = '刚刚';
      else if (diff.inSeconds < 60) lastRefreshAgo = '${diff.inSeconds}秒前';
      else lastRefreshAgo = '${diff.inMinutes}分钟前';
    }

    return StatsModel(
      totalSpots: totalSpots,
      occupiedSpots: occupiedCount,
      zombieSpots: zombieCount,
      occupancyRate: rate,
      hourlyTrend: _buildHourlyTrend(),
      freeSpots: freeCount,
      onlineDevices: onlineDevices,
      gatewayOnline: gatewayOnline, // ⭐ 模拟模式视为在线, 与 onlineDevices 一致
      notifiedToday: notified,
      dispatchedToday: dispatched,
      resolvedToday: resolved,
      avgOccupiedMin: avgOccMin,
      lastRefreshAgo: lastRefreshAgo,
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

  /// 唯一的数据拉取入口: 从 OneNET 拉取完整数据(含模拟), 写 _allSpotsCache,
  /// 再按 _realOnly 本地过滤出 _spots. 切模式不再需要调 API, 0 延迟.
  /// 仅当数据真正变化时才 notifyListeners, 避免 3s 定时无条件触发全 APP 重建
  /// (每次重建都会重新绘制阴影/圆角/图标 → CanvasKit 着色器反复编译 → 卡顿/刷屏).
  Future<void> refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    bool changed = false;
    try {
      final prevById = {for (final s in _spots) s.id: s};
      final prevGatewayOnline = _gatewayOnline;
      // ⭐ 始终拉取完整(含模拟), 写完整缓存 → 切模式仅需本地过滤
      final all = await _apiService.getSpots(realOnly: false);
      _allSpotsCache = List.of(all);
      _allSpotsCacheFetched = true;
      _spots = _realOnly
          ? List.of(all.where((s) => s.isReal))
          : List.of(all);
      _syncLocalState(); // 先回写本地模拟状态(含手动切换), 让自动处理看到一致的当前状态
      /* ⭐⭐⭐ 在任何自动判定之前, 先正确维护每个车位的occupiedSince本地时间戳
       * 这一步是修复"占用时间刷新滞后, 超过30秒阈值"bug的核心: 之后所有的actualOccupiedSec、僵尸判定、
       * 自动通知派单都基于实时计算出的秒数, 不再依赖节点轮询上报 */
      _updateOccupiedSince(prevById);
      // 🆕 【顺序调到最前】先同步节点上报的真实阈值, 再做自动通知/派单
      //    否则首轮刷新用默认1小时阈值判断, 可能导致自动阶梯判定不及时
      await _syncPolicyFromNodes();
      /* ⭐⭐⭐【关键】事件流转必须在【预构建active工单】之前执行!
       *   否则 Case 2 (新车进场变僵尸) 执行 _activeAlerts.remove() 时会误删掉
       *   刚刚为本轮新车预构建出来的active, 导致后续 _buildAlert 生成的alertId
       *   (格式: spotId_occupiedSince) 可能与该车位历史已归档的 alertId
       *   (同一辆车没离开过, occupiedSince没变) 完全相同 → 新旧工单串数据 */
      _handleAutoResolve(prevById); // ① 检测"僵尸车离开"→归档 / "新车变僵尸"→清旧active指针
      /* ⭐⭐⭐【事件流转完成后】再为目前仍为僵尸车的车位预构建 active 工单
       *   此时历史事件指针已清空 → _buildAlert 一定会生成全新的 alertId,
       *   不会和 _resolvedAlerts 里的历史id冲突 */
      for (final spot in _spots.where((s) => s.isZombie)) _buildAlert(spot);
      // 🆕 平台告警策略 2 级自动阶梯:
      await _autoNotifyPendingZombies(); // ① 僵尸告警出现 → 立即自动通知车主(零等待)
      await _autoDispatchOverdueNotified(); // ② 通知后等待dispatchWaitSec未挪车 → 自动派单
      
      /* ⭐ 同时检查网关在线状态 */
      _gatewayOnline = await _apiService.isGatewayOnline();
      changed = _gatewayOnline != prevGatewayOnline || _spotsChanged(prevById);
    } catch (_) {
      // 拉取失败保留上次数据
    }
    final prevLoading = _isLoading;
    _isLoading = false;
    _refreshing = false;
    _lastRefreshAt = DateTime.now();  /* ⭐ 记录成功刷新时间 */
    if (_recordHourlySnapshot()) changed = true; // 整点快照记录也算一次数据变化
    if (prevLoading || changed) notifyListeners();
  }

  /* ⭐⭐⭐ 修复占用时间刷新滞后的核心: 为每个车位正确维护 occupiedSince 本地时间戳
   * 规则:
   * 1. 新占用 (free→occupied): startTime = now - 节点上报的occupiedSec (既保留历史, 后续实时累加)
   * 2. 持续占用: 继承上一轮的 occupiedSince → 时间绝对连续, 刷新不会重置
   * 3. 变空闲: 不设置 (保持 null) */
  void _updateOccupiedSince(Map<String, SpotModel> prevById) {
    final now = DateTime.now();
    for (int i = 0; i < _spots.length; i++) {
      final s = _spots[i];
      if (!(s.isOccupied || s.isZombie)) continue;
      final prev = prevById[s.id];
      // 上一轮也是占用/僵尸, 且有时间戳 → 直接继承, 保证连续
      if (prev != null && (prev.isOccupied || prev.isZombie) && prev.occupiedSince != null) {
        _spots[i] = s.copyWith(occupiedSince: prev.occupiedSince);
      } else {
        // 新车进场 / 首轮拉取 → 用当前时间减去已占用秒数反推准确开始时间
        final startSec = s.occupiedSec > 0 ? s.occupiedSec : 0;
        final startTime = now.subtract(Duration(seconds: startSec));
        _spots[i] = s.copyWith(occupiedSince: startTime);
      }
    }
  }

  /// 对比本轮刷新前后车位数据, 判断是否真的有内容变化 (避免无谓重建).
  bool _spotsChanged(Map<String, SpotModel> prevById) {
    if (prevById.length != _spots.length) return true;
    bool hasOccupied = false;  /* ⭐ 有占用/僵尸车位 → 它的实际占用秒数每轮都在涨, 必须刷新UI */
    for (final s in _spots) {
      if (s.isOccupied || s.isZombie) hasOccupied = true;
      final prev = prevById[s.id];
      if (prev == null) return true;
      if (prev.status != s.status ||
          prev.occupiedHours != s.occupiedHours ||
          prev.notifyStatus != s.notifyStatus ||
          prev.handlerName != s.handlerName ||
          prev.plateNumber != s.plateNumber ||
          prev.signalStrength != s.signalStrength) {  // ⭐ 信号强度变化也要触发UI刷新
        return true;
      }
    }
    /* ⭐ 关键修复: 只要有占用车位, 即使静态字段没变, 占用时长也在实时增长, 需要触发UI重建显示最新秒数 */
    return hasOccupied;
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

  /// ⭐⭐⭐ 同步切换模式(0 延迟): 先本地过滤缓存 → UI 立即切换 → 动画完成后再后台 refresh 同步云端.
  /// 解决: ① 原来 await refresh() 等网络 → 动画前严重卡顿  ② 3s 定时 refresh 抢 _refreshing 锁 → 偶发切失败.
  /// 返回 bool: true=切换成功(含后台刷新已排队), 调用方直接开动画即可.
  bool toggleRealModeSync() {
    _realOnly = !_realOnly;
    SharedPreferences.getInstance().then((p) => p.setBool(_modeKey, _realOnly));
    // ⭐ 缓存已就绪: 纯内存过滤, 0 延迟换数据
    if (_allSpotsCacheFetched) {
      _spots = _realOnly
          ? List.of(_allSpotsCache.where((s) => s.isReal))
          : List.of(_allSpotsCache);
      _syncLocalState(); // 回写模拟覆盖, 防止动画阶段显示被刷新打回
    }
    _skipNextAutoRefresh = true; // 跳过下一轮 3s 定时 refresh, 避免动画期间抢锁 + rebuild
    _modeSwitching = true;        // 暂停 1s tick 的 notify, 防止动画撕裂
    _addLog(
      type: 'mode',
      title: '模式切换',
      detail: _realOnly ? '切换为真实模式（仅真实设备）' : '切换为本地模式（真实 + 模拟）',
    );
    notifyListeners();
    return true;
  }

  /// ⭐ 动画结束后调用: 清除动画保护, 后台异步 refresh 同步云端最新数据 (失败不抛, 静默重试下一轮).
  Future<void> finishModeSwitchAndRefresh() async {
    _modeSwitching = false;
    // 不 await 下一帧 rebuild, 直接跑真实 refresh. 即使失败也不怕, 3s 定时会兜底.
    try {
      await refresh();
    } catch (_) {}
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
    DateTime? occupiedSince;
    switch (spot.status) {
      case 'free':
        next = 'occupied';
        occ = 1;
        occupiedSince = DateTime.now(); // ⭐ 立刻记录当前时间为占用开始, 之后actualOccupiedSec实时从0秒增长
        break;
      case 'occupied':
        next = 'zombie';
        occ = 2;
        // ⭐ 继承上一轮的 occupiedSince → 占用时间绝对连续, 不会跳变
        occupiedSince = spot.occupiedSince ?? DateTime.now().subtract(const Duration(seconds: 30));
        break;
      default: {
        next = 'free';
        occ = 0;
        occupiedSince = null; // ⭐ 车开走, 清空占用开始时间戳
        /* ⭐⭐ 切走=车离开, 主动归档当前 active 工单为 resolved,
         * 让数据中心"今日已解决"实时 +1 (不必等 3s refresh 的 _handleAutoResolve) */
        final active = _activeAlerts[spot.id];
        if (active != null && active.status != 'resolved') {
          final realOcc = spot.actualOccupiedSec; // 切之前(zombie)的占用秒数
          final resolved = active.copyWith(
            status: 'resolved',
            occupiedSec: realOcc,
            occupiedHours: realOcc ~/ 3600,
          );
          _resolvedAlerts[resolved.id] = resolved;
          _handledAts[resolved.id] = DateTime.now();
          _handlerNames.putIfAbsent(resolved.id, () => '自动处理');
          _notifiedAlertIds.remove(resolved.id);
          _dispatchedAlertIds.remove(resolved.id);
          _activeAlerts.remove(spot.id);
          _clearDispatchedAt(spot.id);
          _clearNotifiedAt(resolved.id);
        }
      }
    }
    _spots[idx] = spot.copyWith(
      status: next,
      occupiedHours: occ,
      occupiedSec: next == 'free' ? 0 : spot.occupiedSec,
      occupiedSince: occupiedSince,
    );
    /* ⭐ 切到 zombie 主动生成工单(_activeAlerts), 确保后续通知/派单能拿到 alertId 计入效率卡
     * (之前懒加载依赖 allAlerts 被读取, 车位列表页不读 allAlerts → 通知/派单拿不到 alertId → 效率卡不计数) */
    if (next == 'zombie') {
      _buildAlert(_spots[idx]);
    }
    /* 记录覆盖值, 供 3s 刷新后回写, 防止模拟车位状态被刷新打回默认 */
    _mockStatusOverrides[id] = next;
    _mockOccupiedOverrides[id] = occ;
    notifyListeners();
  }

  /* ==================== 处理动作 (仅本地模拟 stub) ==================== */

  /// 通知车主: 调模拟 stub + 标记对应工单为"已通知"（事件级alertId维度）
  Future<void> notifyOwner(SpotModel spot) async {
    await _apiService.notifyOwner(spot.id);
    final alertId = _activeAlerts[spot.id]?.id;
    if (alertId == null) return;
    _notifiedAlertIds.add(alertId);
    // ⭐ 通知时间戳按【工单id】存，派单倒计时从这一刻开始算
    _notifiedAts[alertId] = DateTime.now();
    await _saveNotifiedAt(alertId); // 持久化前缀也改成alertId
    _syncLocalState();
    _addLog(
      type: 'notify',
      title: '通知车主',
      spotId: spot.id,
      detail: '已通知 ${spot.plateNumber ?? spot.id} 车主尽快挪车，${_formatWaitSec(_policy.dispatchWaitSec)}后未挪车将自动派单',
    );
    notifyListeners();
  }

  /// 派单: 调模拟 stub + 设置处理人 + 标记对应工单为"处理中"（事件级alertId维度）
  Future<void> dispatchSpot(SpotModel spot, {required String handlerName}) async {
    await _apiService.dispatchAlert(spot.id);
    final alertId = _activeAlerts[spot.id]?.id;
    if (alertId == null) return;
    _dispatchedAlertIds.add(alertId);
    _handlerNames[alertId] = handlerName;
    _dispatchedAts[alertId] = DateTime.now();
    _handledAts.remove(alertId); /* 重派时清掉旧完成时间 */
    /* ⭐⭐ 持久化派单状态: 按 spotId 存 (alertId 里的 eventTs 因 occupiedSince 反推不准, spotId 才是稳定标识) */
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt('$_spotDispatchedPrefix${spot.id}', _dispatchedAts[alertId]!.millisecondsSinceEpoch);
    /* 派单后清除通知倒计时(不再需要) */
    await prefs.remove('$_notifiedAtPrefix$alertId');
    _notifiedAts.remove(alertId);
    _syncLocalState();
    _addLog(
      type: 'dispatch',
      title: '派单处理',
      spotId: spot.id,
      detail: '派单给 $handlerName 现场处理',
    );
    notifyListeners();
  }

  /// 批量通知: 并行调 stub + 本地标记"已通知" (事件级alertId维度).
  Future<void> notifySpots(Iterable<SpotModel> spots) async {
    final list = spots.toList();
    await Future.wait(list.map((s) => _apiService.notifyOwner(s.id)));
    final now = DateTime.now();
    for (final s in list) {
      final alertId = _activeAlerts[s.id]?.id;
      if (alertId == null) continue;
      _notifiedAlertIds.add(alertId);
      _notifiedAts[alertId] = now;
      await _saveNotifiedAt(alertId);
    }
    _syncLocalState();
    _addLog(
      type: 'notify',
      title: '批量通知车主',
      detail: '已通知 ${list.length} 个车位的车主挪车，${_formatWaitSec(_policy.dispatchWaitSec)}后未挪车将自动派单',
    );
    notifyListeners();
  }

  /// 批量派单: 并行调 stub + 默认处理人 + 标记"处理中" (事件级alertId维度).
  Future<void> dispatchSpots(Iterable<SpotModel> spots) async {
    final list = spots.toList();
    await Future.wait(list.map((s) => _apiService.dispatchAlert(s.id)));
    final now = DateTime.now();
    final prefs = await SharedPreferences.getInstance();
    for (final s in list) {
      final alertId = _activeAlerts[s.id]?.id;
      if (alertId == null) continue;
      _dispatchedAlertIds.add(alertId);
      _handlerNames.putIfAbsent(alertId, () => '张师傅');
      _dispatchedAts[alertId] = now;
      _handledAts.remove(alertId);
      /* ⭐⭐ 批量派单也要持久化, 按 spotId 存 */
      await prefs.setInt('$_spotDispatchedPrefix${s.id}', now.millisecondsSinceEpoch);
      await prefs.remove('$_notifiedAtPrefix$alertId');
      _notifiedAts.remove(alertId);
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
  /// ⭐ 车位当前是否在【本次事件】处理中: 查active告警id是否在派单集合
  bool isSpotDispatched(String spotId) {
    final alertId = _activeAlerts[spotId]?.id;
    return alertId != null && _dispatchedAlertIds.contains(alertId);
  }

  /// 车位当前告警处理状态: pending=未通知 / notified=已通知 / dispatched=处理中 / resolved=已处理.
  /// ⭐ 事件级隔离: 按【当前active工单】判断, 不是车位级永久标记, 新车进场会变回pending
  String spotAlertStatus(String spotId) {
    final active = _activeAlerts[spotId];
    if (active != null) {
      if (_dispatchedAlertIds.contains(active.id)) return 'dispatched';
      if (_notifiedAlertIds.contains(active.id)) return 'notified';
      return 'pending';
    }
    // 没有active工单 → 若该车位最近1条归档是已处理, 则显示resolved (用于处理完但车位还没清空时)
    final lastResolved = _resolvedAlerts.values.where((a) => a.spotId == spotId).toList();
    if (lastResolved.isNotEmpty) return 'resolved';
    return 'pending';
  }

  /// 🆕 以下 getter 专供【独立告警详情页】读取各阶段时间戳/处理人
  /// ⭐ 查询工单处理时间戳/处理人: 参数改为 alertId（事件级唯一）, 不再按车位绑定
  /// 不再强依赖 SpotModel 实时状态 → 即使车位已空、新车进场, 老工单的处理记录依然完整保留, 不会被覆盖
  DateTime? getNotifiedAt(String alertId) => _notifiedAts[alertId];
  DateTime? getDispatchedAt(String alertId) => _dispatchedAts[alertId];
  DateTime? getHandledAt(String alertId) => _handledAts[alertId];
  String? getHandlerName(String alertId) => _handlerNames[alertId];

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
        final occHours = _mockOccupiedOverrides[s.id] ?? s.occupiedHours;
        s = s.copyWith(
          status: status,
          occupiedHours: occHours,
          occupiedSec: occHours * 3600, // 🆕 模拟车位切换状态时同步秒级时长
        );
      }
    }

    /* ⭐⭐⭐ 所有处理记录改为【事件级 alertId】维度: 查该车位当前active工单的alertId */
    final activeAlert = _activeAlerts[s.id];
    final alertId = activeAlert?.id;

    if (alertId != null) {
      // active工单 = 本次事件正在处理 → 同步它的通知/派单状态、处理人、时间戳到车位展示层
      if (_notifiedAlertIds.contains(alertId)) {
        s.notifyStatus = 'notified';
      }
      final name = _handlerNames[alertId];
      if (name != null) s.handlerName = name;
      final handledAt = _handledAts[alertId];
      if (handledAt != null) s.handledAt = handledAt;
      final notifiedAt = _notifiedAts[alertId];
      if (notifiedAt != null) s.notifiedAt = notifiedAt;
      final dispatchedAt = _dispatchedAts[alertId];
      if (dispatchedAt != null) s.dispatchedAt = dispatchedAt;
    }

    // 🆕 同步告警创建时间:
    // - 活动僵尸车(未处理): 用activeAlert的createdAt, 保证和独立告警详情页完全一致
    // - 已处理历史: 从_resolvedAlerts按spotId找最新的一条历史记录createdAt
    final resolvedList = _resolvedAlerts.values.where((a) => a.spotId == s.id).toList()
      ..sort((a, b) => (b.createdAt ?? DateTime(2000)).compareTo(a.createdAt ?? DateTime(2000)));
    final resolvedAlert = resolvedList.isNotEmpty ? resolvedList.first : null;
    if (activeAlert?.createdAt != null) {
      s.alertCreatedAt = activeAlert!.createdAt;
    } else if (s.isZombie) {
      s.alertCreatedAt ??= DateTime.now().subtract(Duration(seconds: s.actualOccupiedSec));
    } else if (resolvedAlert?.createdAt != null) {
      s.alertCreatedAt = resolvedAlert!.createdAt;
    }
    return s;
  }

  /// ⭐⭐⭐ 【按事件隔离】检测车位状态转换并自动处理（每次refresh后调用）:
  /// ① 僵尸车离开车位 → 把当前active工单归档为【已处理】（按alertId存，不再按spotId覆盖）
  /// ② 新车进场变僵尸 → 删除旧active指针 → 下次_buildAlert生成全新eventId的工单，新老彻底独立
  void _handleAutoResolve(Map<String, SpotModel> prevById) {
    bool wasAlert(SpotModel s) => s.isZombie; // 告警仅在成为僵尸车时产生

    for (final spot in _spots) {
      final prev = prevById[spot.id];
      final prevHadAlert = prev != null && wasAlert(prev);
      final nowHasAlert = wasAlert(spot);

      /* =============== 情况1: 上一轮有告警 → 本轮无 = 车辆离开, 工单结束 =============== */
      if (prevHadAlert && !nowHasAlert) {
        // ⭐ 归档对象使用「active工单」的真实id/createdAt，不是spot.id临时拼的
        final active = _activeAlerts[spot.id];
        final resolvedAt = DateTime.now();

        if (active != null) {
          // active存在 → 直接复用它的 eventId/createdAt/车牌，确保时间线和已处理详情完整
          final realOcc = prev.actualOccupiedSec;
          final resolved = active.copyWith(
            status: 'resolved',
            occupiedSec: realOcc,
            occupiedHours: realOcc ~/ 3600,
          );
          _resolvedAlerts[resolved.id] = resolved; // 🆕 key = alertId（事件级），同一车位多事件不覆盖
          _handledAts[resolved.id] = resolvedAt;    // 完成时间按alertId存
          _handlerNames.putIfAbsent(resolved.id, () => '自动处理');
          // ⭐ 该alertId从 active集合 移除（事件结束），但保留_notifiedAts/dispatchedAts历史供详情查看
          _notifiedAlertIds.remove(resolved.id);
          _dispatchedAlertIds.remove(resolved.id);
          _activeAlerts.remove(spot.id); // ✅ 关键：清掉车位active指针，下一轮新车就会生成全新alertId的工单！
          /* ⭐⭐ 事件结束, 清掉持久化派单状态 */
          _clearDispatchedAt(spot.id);
          _clearNotifiedAt(resolved.id);
        } else {
          // 兜底（active还没生成的极端情况）→ 临时创建一条归档记录
          final realOcc = prev.actualOccupiedSec;
          final temp = AlertModel(
            id: 'alert_${spot.id}_${resolvedAt.millisecondsSinceEpoch}',
            plateNumber: prev.plateNumber ?? '未知车牌',
            spotId: spot.id,
            occupiedHours: realOcc ~/ 3600,
            occupiedSec: realOcc,
            status: 'resolved',
            createdAt: realOcc > 0 ? resolvedAt.subtract(Duration(seconds: realOcc)) : resolvedAt,
          );
          _resolvedAlerts[temp.id] = temp;
          _handledAts[temp.id] = resolvedAt;
          _handlerNames.putIfAbsent(temp.id, () => '自动处理');
          _activeAlerts.remove(spot.id);
        }
      }
      /* =============== 情况2: 新车进场变僵尸 = 开启新一轮事件 =============== */
      else if (prev != null && !prevHadAlert && nowHasAlert) {
        // ⭐ 只有【上一轮可对比】且【上一轮没告警】才判定新车进场
        //    删除旧active指针 → _buildAlert在使用时会生成全新eventId的工单
        //    旧工单已在【情况1】归档到_resolvedAlerts，这里只清当前active，不碰历史
        final oldActive = _activeAlerts.remove(spot.id);
        if (oldActive != null) {
          _notifiedAlertIds.remove(oldActive.id);
          _dispatchedAlertIds.remove(oldActive.id);
          _handlerNames.remove(oldActive.id);
          _handledAts.remove(oldActive.id);
          _dispatchedAts.remove(oldActive.id);
          _clearNotifiedAt(oldActive.id); // 🆕 清除上一轮事件的持久化通知时间戳
          _clearDispatchedAt(spot.id); // ⭐ 清除持久化派单状态 (按 spotId)
        }
      }
    }
  }

  /* ==================== 设备中心 (多网关) ====================
   * 多网关设备中心: 网关列表 + 各网关下挂子设备.
   * 方法转发给 ApiService, 保持"所有平台调用收敛在 Provider"的架构约束. */

  /// 拉取所有网关设备 (网关产品下全部设备 = 网关列表)
  Future<List<Map<String, dynamic>>> getGateways() =>
      _apiService.getGateways();

  /// 拉取指定网关下挂子设备 (OneNET childdevice/list), 解析成 SpotModel
  Future<List<SpotModel>> getGatewayChildren(String gatewayName) =>
      _apiService.getGatewayChildren(gatewayName);

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
  int _otaStallCounter = -1; // ⭐ 升级启动检测: OtaProgress连续0计数(-1=已失效, >=12=60s触发提示)
  bool _otaStallHintShown = false; // ⭐ 升级60s未启动提示(只显示一次)

  String? get otaCurrentVersion => _otaCurrentVersion;
  String? get otaTarget => _otaTarget;
  int? get otaStatus => _otaStatus;
  int get otaStep => _otaProgress;
  bool get otaConfirming => _otaConfirming;
  bool get otaPromptVisible => _otaPromptVisible;
  bool get otaStallHint => _otaStallHintShown; // ⭐ 升级60s未启动提示
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
    _otaStallCounter = 0; // ⭐ 启动升级启动检测: 重新计数
    _otaStallHintShown = false;
    _otaCheckTimer?.cancel(); // 已确认升级: 终止本轮任务检测, 不再继续轮询

    /* ⭐ 前置检查: 网关离线直接拒绝, 避免命令缓存导致 APP 永久"升级中"假死 */
    final gatewayOnline = await _apiService.isGatewayOnline();
    if (!gatewayOnline) {
      _gatewayOnline = false;
      _otaConfirming = false;
      notifyListeners();
      debugPrint('⚠️ 网关离线, 拒绝 OTA 确认');
      return false;
    }
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

    /* ⭐ 升级启动检测: OtaProgress 连续 60s(12轮×5s) 为 0 → 提示"升级似乎未启动".
     * 进度一旦 >0, 计数器永久失效(升级真启动了, 后面再慢都不提示, 避免误杀慢升级). */
    if (_otaStallCounter >= 0) {
      if (_otaProgress > 0) {
        _otaStallCounter = -1; // 进度动了, 永久失效
      } else {
        _otaStallCounter++;
        if (_otaStallCounter >= 12 && !_otaStallHintShown) {
          _otaStallHintShown = true;
          debugPrint('⚠️ OTA 升级 60s 未启动 (OtaProgress 一直为 0)');
        }
      }
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

  /* ==================== 🆕 平台告警策略: 2 级自动阶梯 (纯APP端, 不下发节点) ==================== */

  /// ⭐ 持久化【单个工单】的通知时间戳到 SharedPreferences, key = alertId (事件级).
  Future<void> _saveNotifiedAt(String alertId) async {
    final ts = _notifiedAts[alertId]?.millisecondsSinceEpoch;
    if (ts == null) return;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setInt('$_notifiedAtPrefix$alertId', ts);
  }

  /// ⭐ 清除【单个工单】的通知时间戳缓存 (内存 + SharedPreferences), 事件结束后调用.
  Future<void> _clearNotifiedAt(String alertId) async {
    _notifiedAts.remove(alertId);
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('$_notifiedAtPrefix$alertId');
  }

  /// ⭐ 清除【单个车位】的派单持久化 (按 spotId 清, 因为持久化 key 是 spotId)
  Future<void> _clearDispatchedAt(String spotId) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('$_spotDispatchedPrefix$spotId');
  }

  /// 🆕 ① 僵尸告警出现(pending状态) → 立即自动通知车主, 零等待缓冲.
  Future<void> _autoNotifyPendingZombies() async {
    for (final spot in _spots) {
      if (!spot.isZombie) continue;
      if (spotAlertStatus(spot.id) != 'pending') continue; // 只处理"待通知"的新僵尸车
      // 自动通知, 和手动点"通知车主"效果完全一致
      await notifyOwner(spot);
    }
  }

  /// 🆕 ② 通知车主后等待 dispatchWaitSec 仍未挪车 → 自动派单(默认张师傅), 按【event级alertId】判断
  Future<void> _autoDispatchOverdueNotified() async {
    final now = DateTime.now();
    final waitMs = _policy.dispatchWaitSec * 1000;
    for (final spot in _spots) {
      if (!spot.isZombie) continue; // 车主拖走了(不再是僵尸), 跳过
      final status = spotAlertStatus(spot.id);
      if (status != 'notified') continue; // 只处理"已通知未派单"的
      final alertId = _activeAlerts[spot.id]?.id;
      if (alertId == null) continue;
      final notifiedAt = _notifiedAts[alertId];
      if (notifiedAt == null) continue;
      // 通知时间距今是否超过阈值
      if (now.difference(notifiedAt).inMilliseconds >= waitMs) {
        // 自动派单给默认处理人: 张师傅, 和手动点"派单"效果完全一致
        await dispatchSpot(spot, handlerName: '张师傅');
        _addLog(
          type: 'dispatch',
          title: '系统自动派单',
          spotId: spot.id,
          detail: '通知车主 ${_formatWaitSec(_policy.dispatchWaitSec)} 后未挪车, 自动派单',
          success: true,
        );
      }
    }
  }

  /// 🆕 解决"APP重启变回默认值"bug: 从真实节点拉上报的属性,
  /// 同步到APP本地PolicyConfig (只改本地, 不下发服务, 以节点端实际生效的值为准).
  /// 重启后仅同步1次, 以后3s刷新不再重复覆盖.
  Future<void> _syncPolicyFromNodes() async {
    if (_nodeThresholdSynced) return;
    // 找第一个: 真实节点 + 未停用 + 在线 + 上报了僵尸阈值非null
    // (僵尸阈值是主同步目标, 超声波阈值是额外收益; 只有僵尸阈值非null才设已同步标记)
    for (final s in _spots) {
      if (!s.isReal || s.isDisabledSpot || !s.isOnline) continue;
      final nodeZombie = s.zombieThresholdSec;
      if (nodeZombie == null) continue;  // ← 原始逻辑: 必须僵尸阈值非null才处理
      final nodeSensor = s.sensorDistanceCm;  // 超声波阈值可选同步
      // 与本地对比
      bool changed = false;
      if (nodeZombie != _policy.zombieThresholdSec) {
        debugPrint('[策略同步] 节点${s.id}僵尸阈值=${nodeZombie}s, 本地=${_policy.zombieThresholdSec}s, 自动同步');
        changed = true;
      }
      if (nodeSensor != null && nodeSensor != _policy.sensorDistanceCm) {
        debugPrint('[策略同步] 节点${s.id}超声波距离=${nodeSensor}cm, 本地=${_policy.sensorDistanceCm}cm, 自动同步');
        changed = true;
      }
      if (changed) {
        _policy = _policy.copyWith(
          zombieThresholdSec: nodeZombie,
          sensorDistanceCm: nodeSensor ?? _policy.sensorDistanceCm,
        );
        await _policy.save();
        _addLog(
          type: 'policy',
          title: '策略自动同步',
          spotId: s.id,
          detail: '检测到节点阈值与本地不一致, 已自动同步(僵尸=${nodeZombie}s, 超声波=${nodeSensor}cm)',
          success: true,
        );
      }
      _nodeThresholdSynced = true;
      notifyListeners();
      return;
    }
    // 没找到合法僵尸阈值(节点离线/未上报), 下次刷新继续尝试, 不设标记
  }

  /// 🆕 策略配置页手动一键同步: 把节点端真实阈值写进APP本地PolicyConfig(不下发服务).
  Future<bool> syncNodeThresholdToLocal() async {
    for (final s in _spots) {
      if (!s.isReal || s.isDisabledSpot || !s.isOnline) continue;
      final nodeZombie = s.zombieThresholdSec;
      final nodeSensor = s.sensorDistanceCm;
      if (nodeZombie == null && nodeSensor == null) continue;
      _policy = _policy.copyWith(
        zombieThresholdSec: nodeZombie ?? _policy.zombieThresholdSec,
        sensorDistanceCm: nodeSensor ?? _policy.sensorDistanceCm,
      );
      await _policy.save();
      _nodeThresholdSynced = true;
      _addLog(
        type: 'policy',
        title: '手动同步阈值',
        spotId: s.id,
        detail: '已将节点端真实阈值同步(僵尸=${nodeZombie}s, 超声波=${nodeSensor}cm)',
        success: true,
      );
      notifyListeners();
      return true;
    }
    _policyError = '节点离线或未上报阈值，无法同步';
    notifyListeners();
    return false;
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    _tickTimer?.cancel(); // ⭐ 1秒粒度秒数跳动定时器
    _otaCheckTimer?.cancel();
    _otaPollTimer?.cancel();
    _apiService.dispose();
    super.dispose();
  }
}
