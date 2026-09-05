import 'package:shared_preferences/shared_preferences.dart';

/// 策略配置: 把系统里写死的判定规则集中于此, 可在 App 内修改并持久化, 修改即时生效.
///
/// 五组策略:
/// - 告警策略: 停车超时告警阈值 (秒级)
/// - ⭐ 僵尸车判定阈值: 节点端判定僵尸车的秒级阈值 (节点端下发, 不要改含义)
/// - 报警灯策略: 节点僵尸车报警灯使能开关 (节点策略, 经 SetLed 服务下发, 平台不保存该开关态)
/// - 🆕 平台派单策略: 通知车主后等待多少秒仍未挪车 → 自动派单 (纯APP端, 不下发节点)
/// - 传感器策略: 传感器矛盾判定距离阈值
/// - 刷新策略: 数据轮询刷新间隔
/// - OTA 策略: 自动检测开关 / 检测间隔 / 每轮检测次数
class PolicyConfig {
  final int alertSec; // 停车超时告警阈值 (秒): 占用超过该时长生成告警
  final int zombieThresholdSec; // ⭐ 僵尸车判定阈值 (秒): 节点端占用超过该时长判定为僵尸车
  final bool ledAlarmEnabled; // ⭐ 报警灯使能 (僵尸车报警灯): 节点策略, 经 SetLed(LedState) 下发
  final int dispatchWaitSec; // 🆕 平台派单等待时间 (秒): 通知车主后等待多久未挪车自动派单 (默认24h=86400s)
  final int sensorDistanceCm; // 传感器矛盾判定距离阈值 (cm): 与节点固件 DIST_THRESHOLD_CM 对应
  final int refreshSec; // 数据刷新间隔 (秒)
  final bool otaEnabled; // OTA 自动检测开关
  final int otaIntervalSec; // OTA 每轮检测间隔 (秒)
  final int otaCheckCount; // OTA 每轮检测次数 (进入前台立即 1 次 + 剩余按间隔补查)
  // 🆕 自动拍照策略 (纯APP端, 不下发设备): 检测到车位状态事件后自动调摄像头拍照识别车牌
  final bool autoCaptureEnabled; // 自动拍照总开关
  final int autoCaptureMode;     // 拍照时机: 0=有车就拍  1=僵尸车才拍

  const PolicyConfig({
    this.alertSec = 3600, // 默认1小时
    this.zombieThresholdSec = 3600, // 默认1小时, 可设置短阈值用于演示
    this.ledAlarmEnabled = true, // ⭐ 默认开启, 与节点端出厂默认一致
    this.dispatchWaitSec = 86400, // 🆕 默认24小时, 通知车主后1天未挪车自动派单
    this.sensorDistanceCm = 10,
    this.refreshSec = 3,
    this.otaEnabled = true,
    this.otaIntervalSec = 5,
    this.otaCheckCount = 10,
    this.autoCaptureEnabled = false, // 🆕 默认关闭
    this.autoCaptureMode = 1,        // 🆕 默认"僵尸车才拍"(更省OCR额度, 也贴合自动取证诉求)
  });

  /// 出厂默认值 (用于"恢复默认设置").
  static const PolicyConfig defaults = PolicyConfig();

  static const _kAlertSec = 'policy_alert_sec';
  static const _kZombieThresholdSec = 'policy_zombie_threshold_sec';
  static const _kLedAlarmEnabled = 'policy_led_alarm_enabled';
  static const _kDispatchWaitSec = 'policy_dispatch_wait_sec';
  static const _kSensorCm = 'policy_sensor_distance_cm';
  static const _kRefreshSec = 'policy_refresh_sec';
  static const _kOtaEnabled = 'policy_ota_enabled';
  static const _kOtaIntervalSec = 'policy_ota_interval_sec';
  static const _kOtaCheckCount = 'policy_ota_check_count';
  static const _kAutoCaptureEnabled = 'policy_auto_capture_enabled';
  static const _kAutoCaptureMode = 'policy_auto_capture_mode';

  /// 从 SharedPreferences 加载 (无记录时使用默认值).
  static Future<PolicyConfig> load() async {
    final p = await SharedPreferences.getInstance();
    return PolicyConfig(
      alertSec: p.getInt(_kAlertSec) ?? defaults.alertSec,
      zombieThresholdSec: p.getInt(_kZombieThresholdSec) ?? defaults.zombieThresholdSec,
      ledAlarmEnabled: p.getBool(_kLedAlarmEnabled) ?? defaults.ledAlarmEnabled,
      dispatchWaitSec: p.getInt(_kDispatchWaitSec) ?? defaults.dispatchWaitSec,
      sensorDistanceCm: p.getInt(_kSensorCm) ?? defaults.sensorDistanceCm,
      refreshSec: p.getInt(_kRefreshSec) ?? defaults.refreshSec,
      otaEnabled: p.getBool(_kOtaEnabled) ?? defaults.otaEnabled,
      otaIntervalSec: p.getInt(_kOtaIntervalSec) ?? defaults.otaIntervalSec,
      otaCheckCount: p.getInt(_kOtaCheckCount) ?? defaults.otaCheckCount,
      autoCaptureEnabled: p.getBool(_kAutoCaptureEnabled) ?? defaults.autoCaptureEnabled,
      autoCaptureMode: p.getInt(_kAutoCaptureMode) ?? defaults.autoCaptureMode,
    );
  }

  /// 持久化到 SharedPreferences.
  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setInt(_kAlertSec, alertSec);
    await p.setInt(_kZombieThresholdSec, zombieThresholdSec);
    await p.setBool(_kLedAlarmEnabled, ledAlarmEnabled);
    await p.setInt(_kDispatchWaitSec, dispatchWaitSec);
    await p.setInt(_kSensorCm, sensorDistanceCm);
    await p.setInt(_kRefreshSec, refreshSec);
    await p.setBool(_kOtaEnabled, otaEnabled);
    await p.setInt(_kOtaIntervalSec, otaIntervalSec);
    await p.setInt(_kOtaCheckCount, otaCheckCount);
    await p.setBool(_kAutoCaptureEnabled, autoCaptureEnabled);
    await p.setInt(_kAutoCaptureMode, autoCaptureMode);
  }

  PolicyConfig copyWith({
    int? alertSec,
    int? zombieThresholdSec,
    bool? ledAlarmEnabled,
    int? dispatchWaitSec,
    int? sensorDistanceCm,
    int? refreshSec,
    bool? otaEnabled,
    int? otaIntervalSec,
    int? otaCheckCount,
    bool? autoCaptureEnabled,
    int? autoCaptureMode,
  }) {
    return PolicyConfig(
      alertSec: alertSec ?? this.alertSec,
      zombieThresholdSec: zombieThresholdSec ?? this.zombieThresholdSec,
      ledAlarmEnabled: ledAlarmEnabled ?? this.ledAlarmEnabled,
      dispatchWaitSec: dispatchWaitSec ?? this.dispatchWaitSec,
      sensorDistanceCm: sensorDistanceCm ?? this.sensorDistanceCm,
      refreshSec: refreshSec ?? this.refreshSec,
      otaEnabled: otaEnabled ?? this.otaEnabled,
      otaIntervalSec: otaIntervalSec ?? this.otaIntervalSec,
      otaCheckCount: otaCheckCount ?? this.otaCheckCount,
      autoCaptureEnabled: autoCaptureEnabled ?? this.autoCaptureEnabled,
      autoCaptureMode: autoCaptureMode ?? this.autoCaptureMode,
    );
  }
}
