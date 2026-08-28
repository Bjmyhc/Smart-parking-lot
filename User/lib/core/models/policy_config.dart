import 'package:shared_preferences/shared_preferences.dart';

/// 策略配置: 把系统里写死的判定规则集中于此, 可在 App 内修改并持久化, 修改即时生效.
///
/// 四组策略:
/// - 告警策略: 停车超时告警阈值 (秒级)
/// - ⭐ 僵尸车判定阈值: 节点端判定僵尸车的秒级阈值
/// - 传感器策略: 传感器矛盾判定距离阈值
/// - 刷新策略: 数据轮询刷新间隔
/// - OTA 策略: 自动检测开关 / 检测间隔 / 每轮检测次数
class PolicyConfig {
  final int alertSec; // 停车超时告警阈值 (秒): 占用超过该时长生成告警
  final int zombieThresholdSec; // ⭐ 僵尸车判定阈值 (秒): 节点端占用超过该时长判定为僵尸车
  final int sensorDistanceCm; // 传感器矛盾判定距离阈值 (cm): 与节点固件 DIST_THRESHOLD_CM 对应
  final int refreshSec; // 数据刷新间隔 (秒)
  final bool otaEnabled; // OTA 自动检测开关
  final int otaIntervalSec; // OTA 每轮检测间隔 (秒)
  final int otaCheckCount; // OTA 每轮检测次数 (进入前台立即 1 次 + 剩余按间隔补查)

  const PolicyConfig({
    this.alertSec = 3600, // 默认1小时
    this.zombieThresholdSec = 3600, // 默认1小时, 可设置短阈值用于演示
    this.sensorDistanceCm = 10,
    this.refreshSec = 3,
    this.otaEnabled = true,
    this.otaIntervalSec = 5,
    this.otaCheckCount = 10,
  });

  /// 出厂默认值 (用于"恢复默认设置").
  static const PolicyConfig defaults = PolicyConfig();

  static const _kAlertSec = 'policy_alert_sec';
  static const _kZombieThresholdSec = 'policy_zombie_threshold_sec';
  static const _kSensorCm = 'policy_sensor_distance_cm';
  static const _kRefreshSec = 'policy_refresh_sec';
  static const _kOtaEnabled = 'policy_ota_enabled';
  static const _kOtaIntervalSec = 'policy_ota_interval_sec';
  static const _kOtaCheckCount = 'policy_ota_check_count';

  /// 从 SharedPreferences 加载 (无记录时使用默认值).
  static Future<PolicyConfig> load() async {
    final p = await SharedPreferences.getInstance();
    return PolicyConfig(
      alertSec: p.getInt(_kAlertSec) ?? defaults.alertSec,
      zombieThresholdSec: p.getInt(_kZombieThresholdSec) ?? defaults.zombieThresholdSec,
      sensorDistanceCm: p.getInt(_kSensorCm) ?? defaults.sensorDistanceCm,
      refreshSec: p.getInt(_kRefreshSec) ?? defaults.refreshSec,
      otaEnabled: p.getBool(_kOtaEnabled) ?? defaults.otaEnabled,
      otaIntervalSec: p.getInt(_kOtaIntervalSec) ?? defaults.otaIntervalSec,
      otaCheckCount: p.getInt(_kOtaCheckCount) ?? defaults.otaCheckCount,
    );
  }

  /// 持久化到 SharedPreferences.
  Future<void> save() async {
    final p = await SharedPreferences.getInstance();
    await p.setInt(_kAlertSec, alertSec);
    await p.setInt(_kZombieThresholdSec, zombieThresholdSec);
    await p.setInt(_kSensorCm, sensorDistanceCm);
    await p.setInt(_kRefreshSec, refreshSec);
    await p.setBool(_kOtaEnabled, otaEnabled);
    await p.setInt(_kOtaIntervalSec, otaIntervalSec);
    await p.setInt(_kOtaCheckCount, otaCheckCount);
  }

  PolicyConfig copyWith({
    int? alertSec,
    int? zombieThresholdSec,
    int? sensorDistanceCm,
    int? refreshSec,
    bool? otaEnabled,
    int? otaIntervalSec,
    int? otaCheckCount,
  }) {
    return PolicyConfig(
      alertSec: alertSec ?? this.alertSec,
      zombieThresholdSec: zombieThresholdSec ?? this.zombieThresholdSec,
      sensorDistanceCm: sensorDistanceCm ?? this.sensorDistanceCm,
      refreshSec: refreshSec ?? this.refreshSec,
      otaEnabled: otaEnabled ?? this.otaEnabled,
      otaIntervalSec: otaIntervalSec ?? this.otaIntervalSec,
      otaCheckCount: otaCheckCount ?? this.otaCheckCount,
    );
  }
}
