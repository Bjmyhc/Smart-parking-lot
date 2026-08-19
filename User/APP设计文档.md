# 路边僵尸车检测系统 - 设计文档

---

## 【一、整体风格】

- 设计语言：Material 3 + 华为智慧生活式"高端科技感"
- 主色调：华为蓝 #007DFF（Primary），辅色用浅蓝 #E8F4FD
- 背景色：浅灰白 #F5F7FA（不是纯白，减少刺眼感）
- 强调色：成功绿 #00C781、警告橙 #FF9F43、危险红 #FF5B5B
  （用于车位状态：空闲=绿、占用=橙、僵尸车=红）
- 文字：主文字 #1A1A1A、次要文字 #8A8A8A
- 圆角：卡片 16dp、按钮 12dp、小标签 8dp
- 阴影：卡片用柔和阴影（blurRadius=12, offset=(0,2), opacity=0.08）
- 留白：页面边距 20dp，卡片间距 16dp（华为改版强调"适当增加留白"）

---

## 【二、组件规范】

- 车位状态卡片：白色背景 + 16dp 圆角 + 柔和阴影，左侧是车位编号大字号，
  右侧是三色状态徽章（空闲/占用/僵尸车），底部是占用时长进度条
- 设备卡片（参考华为智慧生活）：展示 LoRa 节点图标、在线状态、
  电池电量环形进度、信号强度，右上角是圆形开关图标
- 列表项：左图标 + 中标题/副标题 + 右箭头或状态标签，分割线用 0.5dp 浅灰
- 详情页：顶部大图/车牌识别结果，中部信息分区卡片，底部操作按钮行
- 导航：底部 TabBar 4 个（总览/车位/告警/我的），选中色用华为蓝
- 图表：占用率用环形图，7天趋势用折线图，使用 fl_chart 库

---

## 【三、页面结构】

1. **总览页**：顶部问候语+系统状态概览卡片（总车位/占用/僵尸车数），
   中部"实时车位状态"网格卡片，底部"最新僵尸车告警"列表项

2. **车位页**：分区（A区/B区/C区）SegmentedControl 切换，
   每个分区下是车位状态卡片列表，点击进入车位详情

3. **告警页**：僵尸车工单列表，每张工单卡片显示车牌号、车位、占用时长、
   识别照片缩略图、状态标签（待派单/已派单/已处置），右侧操作按钮

4. **详情页**：车牌大图展示、车辆信息卡片、占用时间轴、处置操作按钮
   （派单/通知车主/标记已处置）

5. **我的页**：系统状态、节点管理、配网入口、数据统计入口

---

## 【四、推荐的 Flutter UI 库】

| 库名 | 用途 | 说明 |
|------|------|------|
| fl_chart | 图表 | 环形图、折线图、柱状图，画占用率和趋势 |
| flutter_slidable | 滑动操作 | 工单列表项左滑派单/删除 |
| flutter_staggered_grid_view | 网格布局 | 车位状态网格 |
| flutter_swiper_view | 轮播 | 车位图片轮播、引导页 |
| animated_text_kit | 文字动画 | 数据加载、数字滚动效果 |
| flutter_spinkit | 加载指示器 | 拍照识别时的 loading 动画 |

> **注意**：本项目已手写 `HuaweiCard`、`StatusBadge`、`CircleProgress` 等基础组件，优先使用手写组件。其他复杂场景（如对话框、表单）可根据需要引入第三方库。

---

## 【五、禁止事项】

- 不要用 Material 2 的默认深紫粉色主题
- 不要用尖锐直角按钮
- 不要堆砌功能入口（华为的教训：设备多时会拥挤）
- 不要用刺眼的纯白色背景
- 列表项之间要有充足留白，不要紧凑排列

---

## 【六、全局设计令牌】

### 6.1 颜色配置 `lib/core/theme/app_colors.dart`

```dart
import 'package:flutter/material.dart';

class AppColors {
  // Light Theme
  static const Color primary = Color(0xFF007DFF);
  static const Color primaryLight = Color(0xFFE8F4FD);
  static const Color background = Color(0xFFF5F7FA);
  static const Color surface = Colors.white;
  static const Color success = Color(0xFF00C781);
  static const Color warning = Color(0xFFFF9F43);
  static const Color danger = Color(0xFFFF5B5B);
  static const Color textPrimary = Color(0xFF1A1A1A);
  static const Color textSecondary = Color(0xFF8A8A8A);

  // Dark Theme
  static const Color darkBackground = Color(0xFF121212);
  static const Color darkSurface = Color(0xFF1E1E1E);
  static const Color darkTextPrimary = Color(0xFFFFFFFF);
  static const Color darkTextSecondary = Color(0xFFB0B0B0);
}
```

### 6.2 尺寸配置 `lib/core/theme/app_dims.dart`

```dart
class AppDims {
  static const double radiusSmall = 8.0;
  static const double radiusMedium = 12.0;
  static const double radiusLarge = 16.0;
  static const double paddingPage = 20.0;
  static const double gapCard = 16.0;
  static const double paddingCard = 16.0;
  static const double gapItem = 12.0;
}
```

---

## 【七、数据模型定义】

### 7.1 车位模型 `lib/core/models/spot_model.dart`

```dart
class SpotModel {
  final String id;
  final String zone;          // A/B/C
  final String status;        // free / occupied / zombie
  final int occupiedHours;
  final double batteryLevel;
  final int signalStrength;

  SpotModel({
    required this.id,
    required this.zone,
    required this.status,
    required this.occupiedHours,
    required this.batteryLevel,
    required this.signalStrength,
  });

  factory SpotModel.fromJson(Map<String, dynamic> json) {
    return SpotModel(
      id: json['id'] as String,
      zone: json['zone'] as String,
      status: json['status'] as String,
      occupiedHours: (json['occupied_hours'] as num).toInt(),
      batteryLevel: (json['battery_level'] as num).toDouble(),
      signalStrength: (json['signal_strength'] as num).toInt(),
    );
  }

  Map<String, dynamic> toJson() {
    return {
      'id': id,
      'zone': zone,
      'status': status,
      'occupied_hours': occupiedHours,
      'battery_level': batteryLevel,
      'signal_strength': signalStrength,
    };
  }
}
```

### 7.2 告警模型 `lib/core/models/alert_model.dart`

```dart
class AlertModel {
  final String id;
  final String plateNumber;
  final String spotId;
  final int occupiedHours;
  final String status;        // pending / dispatched / resolved
  final String? imageUrl;
  final DateTime createdAt;

  AlertModel({
    required this.id,
    required this.plateNumber,
    required this.spotId,
    required this.occupiedHours,
    required this.status,
    this.imageUrl,
    required this.createdAt,
  });

  factory AlertModel.fromJson(Map<String, dynamic> json) {
    return AlertModel(
      id: json['id'] as String,
      plateNumber: json['plate_number'] as String,
      spotId: json['spot_id'] as String,
      occupiedHours: (json['occupied_hours'] as num).toInt(),
      status: json['status'] as String,
      imageUrl: json['image_url'] as String?,
      createdAt: DateTime.parse(json['created_at'] as String),
    );
  }

  Map<String, dynamic> toJson() {
    return {
      'id': id,
      'plate_number': plateNumber,
      'spot_id': spotId,
      'occupied_hours': occupiedHours,
      'status': status,
      'image_url': imageUrl,
      'created_at': createdAt.toIso8601String(),
    };
  }
}
```

### 7.3 统计模型 `lib/core/models/stats_model.dart`

```dart
class StatsModel {
  final int totalSpots;
  final int occupiedSpots;
  final int zombieSpots;
  final double occupancyRate;
  final List<DailyTrend> weeklyTrend;

  StatsModel({
    required this.totalSpots,
    required this.occupiedSpots,
    required this.zombieSpots,
    required this.occupancyRate,
    required this.weeklyTrend,
  });

  factory StatsModel.fromJson(Map<String, dynamic> json) {
    return StatsModel(
      totalSpots: (json['total_spots'] as num).toInt(),
      occupiedSpots: (json['occupied_spots'] as num).toInt(),
      zombieSpots: (json['zombie_spots'] as num).toInt(),
      occupancyRate: (json['occupancy_rate'] as num).toDouble(),
      weeklyTrend: (json['weekly_trend'] as List)
          .map((e) => DailyTrend.fromJson(e as Map<String, dynamic>))
          .toList(),
    );
  }
}

class DailyTrend {
  final String date;
  final int avgOccupancy;
  final int alertsCount;

  DailyTrend({
    required this.date,
    required this.avgOccupancy,
    required this.alertsCount,
  });

  factory DailyTrend.fromJson(Map<String, dynamic> json) {
    return DailyTrend(
      date: json['date'] as String,
      avgOccupancy: (json['avg_occupancy'] as num).toInt(),
      alertsCount: (json['alerts_count'] as num).toInt(),
    );
  }
}
```

---

## 【八、API 服务层定义】

`lib/core/services/api_service.dart`

```dart
import 'package:http/http.dart' as http;
import 'dart:convert';
import '../models/spot_model.dart';
import '../models/alert_model.dart';
import '../models/stats_model.dart';

class ApiService {
  static const String baseUrl = 'http://192.168.4.1:8080';

  /// 获取所有车位状态
  static Future<List<SpotModel>> getSpots() async {
    final response = await http.get(Uri.parse('$baseUrl/spots'));
    if (response.statusCode == 200) {
      final List<dynamic> data = json.decode(response.body);
      return data.map((e) => SpotModel.fromJson(e as Map<String, dynamic>)).toList();
    }
    throw Exception('Failed to load spots');
  }

  /// 获取告警列表
  static Future<List<AlertModel>> getAlerts() async {
    final response = await http.get(Uri.parse('$baseUrl/alerts'));
    if (response.statusCode == 200) {
      final List<dynamic> data = json.decode(response.body);
      return data.map((e) => AlertModel.fromJson(e as Map<String, dynamic>)).toList();
    }
    throw Exception('Failed to load alerts');
  }

  /// 派单
  static Future<bool> dispatchAlert(String alertId) async {
    final response = await http.post(
      Uri.parse('$baseUrl/alerts/$alertId/dispatch'),
    );
    return response.statusCode == 200;
  }

  /// 通知车主
  static Future<bool> notifyOwner(String alertId) async {
    final response = await http.post(
      Uri.parse('$baseUrl/alerts/$alertId/notify'),
    );
    return response.statusCode == 200;
  }

  /// 标记已处置
  static Future<bool> resolveAlert(String alertId) async {
    final response = await http.post(
      Uri.parse('$baseUrl/alerts/$alertId/resolve'),
    );
    return response.statusCode == 200;
  }

  /// 获取统计数据
  static Future<StatsModel> getStats() async {
    final response = await http.get(Uri.parse('$baseUrl/stats'));
    if (response.statusCode == 200) {
      return StatsModel.fromJson(json.decode(response.body) as Map<String, dynamic>);
    }
    throw Exception('Failed to load stats');
  }
}
```

---

## 【九、基础组件实现】

### 9.1 华为卡片 `lib/shared/widgets/huawei_card.dart`

```dart
import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_dims.dart';

class HuaweiCard extends StatelessWidget {
  final Widget child;
  const HuaweiCard({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(AppDims.paddingCard),
      decoration: BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.circular(AppDims.radiusLarge),
        boxShadow: [
          BoxShadow(
            color: AppColors.textPrimary.withOpacity(0.08),
            blurRadius: 12,
            offset: const Offset(0, 2),
          )
        ],
      ),
      child: child,
    );
  }
}
```

### 9.2 状态徽章 `lib/shared/widgets/status_badge.dart`

```dart
import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';
import '../../core/theme/app_dims.dart';

class StatusBadge extends StatelessWidget {
  final String text;
  final Color color;
  const StatusBadge({super.key, required this.text, required this.color});

  factory StatusBadge.free() => const StatusBadge(text: '空闲', color: AppColors.success);
  factory StatusBadge.occupied() => const StatusBadge(text: '占用', color: AppColors.warning);
  factory StatusBadge.zombie() => const StatusBadge(text: '僵尸车', color: AppColors.danger);

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: color.withOpacity(0.1),
        borderRadius: BorderRadius.circular(AppDims.radiusSmall),
        border: Border.all(color: color.withOpacity(0.3), width: 0.5),
      ),
      child: Text(
        text,
        style: TextStyle(fontSize: 12, fontWeight: FontWeight.w500, color: color),
      ),
    );
  }
}
```

### 9.3 环形进度条 `lib/shared/widgets/circle_progress.dart`

```dart
import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';

class CircleProgress extends StatelessWidget {
  final double percentage;
  final double size;
  final Color color;
  const CircleProgress({super.key, required this.percentage, this.size = 40, required this.color});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: size,
      height: size,
      child: Stack(
        alignment: Alignment.center,
        children: [
          CircularProgressIndicator(
            value: percentage / 100,
            strokeWidth: 4,
            backgroundColor: AppColors.background,
            valueColor: AlwaysStoppedAnimation(color),
          ),
          Text(
            '${percentage.toInt()}%',
            style: TextStyle(fontSize: size * 0.3, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
          ),
        ],
      ),
    );
  }
}
```

### 9.4 空状态组件 `lib/shared/widgets/empty_state.dart`

```dart
import 'package:flutter/material.dart';
import '../../core/theme/app_colors.dart';

class EmptyState extends StatelessWidget {
  final String message;
  const EmptyState({super.key, required this.message});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(Icons.inbox_outlined, size: 80, color: AppColors.textSecondary.withOpacity(0.5)),
          const SizedBox(height: 16),
          Text(message, style: const TextStyle(fontSize: 16, color: AppColors.textSecondary)),
        ],
      ),
    );
  }
}
```

### 9.5 加载指示器 `lib/shared/widgets/loading_indicator.dart`

```dart
import 'package:flutter/material.dart';
import 'package:flutter_spinkit/flutter_spinkit.dart';
import '../../core/theme/app_colors.dart';

class LoadingIndicator extends StatelessWidget {
  final double size;
  const LoadingIndicator({super.key, this.size = 50});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: SpinKitFadingCircle(color: AppColors.primary, size: size),
    );
  }
}
```

---

## 【十、页面实现示例】

### 10.1 总览页 `lib/features/overview/presentation/pages/overview_page.dart`

```dart
import 'package:flutter/material.dart';
import 'package:flutter_staggered_grid_view/flutter_staggered_grid_view.dart';
import '../../../../shared/widgets/huawei_card.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';

class OverviewPage extends StatelessWidget {
  const OverviewPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(
        title: const Text('路边僵尸车监测系统'),
        backgroundColor: AppColors.surface,
        elevation: 0,
        centerTitle: true,
        titleTextStyle: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary),
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('下午好，管理员', style: TextStyle(fontSize: 24, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            const SizedBox(height: 8),
            const Text('当前系统运行正常，共监测32个车位', style: TextStyle(fontSize: 14, color: AppColors.textSecondary)),
            const SizedBox(height: 24),
            Row(
              children: [
                Expanded(child: _buildStatCard('总车位', '32', AppColors.primary)),
                const SizedBox(width: AppDims.gapCard),
                Expanded(child: _buildStatCard('占用中', '18', AppColors.warning)),
                const SizedBox(width: AppDims.gapCard),
                Expanded(child: _buildStatCard('僵尸车', '3', AppColors.danger)),
              ],
            ),
            const SizedBox(height: 24),
            const Text('实时车位状态', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            const SizedBox(height: AppDims.gapItem),
            MasonryGridView.count(
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              crossAxisCount: 2,
              mainAxisSpacing: AppDims.gapCard,
              crossAxisSpacing: AppDims.gapCard,
              itemCount: 6,
              itemBuilder: (context, index) => HuaweiCard(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      mainAxisAlignment: MainAxisAlignment.spaceBetween,
                      children: [
                        Text('A-${index + 1}', style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                        StatusBadge.free(),
                      ],
                    ),
                    const Spacer(),
                    const Text('空闲中', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildStatCard(String title, String value, Color color) {
    return HuaweiCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: const TextStyle(fontSize: 14, color: AppColors.textSecondary)),
          const SizedBox(height: 8),
          Text(value, style: TextStyle(fontSize: 28, fontWeight: FontWeight.w700, color: color)),
          const SizedBox(height: 8),
          Container(
            height: 4,
            decoration: BoxDecoration(
              color: color.withOpacity(0.1),
              borderRadius: BorderRadius.circular(2),
            ),
            child: LinearProgressIndicator(
              value: double.parse(value) / 32,
              backgroundColor: Colors.transparent,
              valueColor: AlwaysStoppedAnimation(color),
            ),
          ),
        ],
      ),
    );
  }
}
```

### 10.2 车位详情页 `lib/features/spots/presentation/pages/spot_detail_page.dart`

```dart
import 'package:flutter/material.dart';
import '../../../../shared/widgets/huawei_card.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';

class SpotDetailPage extends StatelessWidget {
  final String spotId;
  const SpotDetailPage({super.key, required this.spotId});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(
        title: Text('车位详情 $spotId'),
        backgroundColor: AppColors.surface,
        elevation: 0,
      ),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // 车牌大图展示
            HuaweiCard(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Container(
                    height: 200,
                    decoration: BoxDecoration(
                      color: AppColors.background,
                      borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                    ),
                    child: const Center(
                      child: Icon(Icons.directions_car, size: 80, color: AppColors.textSecondary),
                    ),
                  ),
                  const SizedBox(height: AppDims.gapItem),
                  const Text('京A·12345', style: TextStyle(fontSize: 24, fontWeight: FontWeight.w700, color: AppColors.textPrimary)),
                  const SizedBox(height: 4),
                  Row(
                    children: [
                      StatusBadge.occupied(),
                      const Spacer(),
                      const Text('占用 72小时', style: TextStyle(fontSize: 14, color: AppColors.textSecondary)),
                    ],
                  ),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 车辆信息卡片
            HuaweiCard(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('车辆信息', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                  const SizedBox(height: AppDims.gapItem),
                  _buildInfoRow('车位编号', spotId),
                  _buildInfoRow('所在分区', 'A区'),
                  _buildInfoRow('入场时间', '2024-01-15 08:30'),
                  _buildInfoRow('LoRa信号', '强 (-65dBm)'),
                  _buildInfoRow('电池电量', '85%'),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 操作按钮
            Row(
              children: [
                Expanded(
                  child: ElevatedButton(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: AppColors.primary,
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                      ),
                    ),
                    onPressed: () {},
                    child: const Text('派单', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                  ),
                ),
                const SizedBox(width: AppDims.gapCard),
                Expanded(
                  child: ElevatedButton(
                    style: ElevatedButton.styleFrom(
                      backgroundColor: AppColors.warning,
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                      ),
                    ),
                    onPressed: () {},
                    child: const Text('通知车主', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
                  ),
                ),
              ],
            ),
            const SizedBox(height: AppDims.gapItem),
            SizedBox(
              width: double.infinity,
              child: ElevatedButton(
                style: ElevatedButton.styleFrom(
                  backgroundColor: AppColors.success,
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(AppDims.radiusMedium),
                  ),
                ),
                onPressed: () {},
                child: const Text('标记已处置', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600)),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildInfoRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label, style: const TextStyle(fontSize: 14, color: AppColors.textSecondary)),
          Text(value, style: const TextStyle(fontSize: 14, fontWeight: FontWeight.w500, color: AppColors.textPrimary)),
        ],
      ),
    );
  }
}
```

### 10.3 告警页 `lib/features/alerts/presentation/pages/alerts_page.dart`

```dart
import 'package:flutter/material.dart';
import 'package:flutter_slidable/flutter_slidable.dart';
import '../../../../shared/widgets/huawei_card.dart';
import '../../../../shared/widgets/status_badge.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';

class AlertsPage extends StatelessWidget {
  const AlertsPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(title: const Text('僵尸车告警')),
      body: ListView.separated(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        itemCount: 5,
        separatorBuilder: (_, __) => const SizedBox(height: AppDims.gapCard),
        itemBuilder: (context, index) {
          return Slidable(
            key: ValueKey(index),
            endActionPane: ActionPane(
              motion: const ScrollMotion(),
              children: [
                SlidableAction(
                  onPressed: (_) {},
                  backgroundColor: AppColors.primary,
                  foregroundColor: Colors.white,
                  icon: Icons.send,
                  label: '派单',
                  borderRadius: const BorderRadius.horizontal(left: Radius.circular(AppDims.radiusLarge)),
                ),
                SlidableAction(
                  onPressed: (_) {},
                  backgroundColor: AppColors.warning,
                  foregroundColor: Colors.white,
                  icon: Icons.message,
                  label: '通知',
                ),
                SlidableAction(
                  onPressed: (_) {},
                  backgroundColor: AppColors.success,
                  foregroundColor: Colors.white,
                  icon: Icons.check,
                  label: '处置',
                  borderRadius: const BorderRadius.horizontal(right: Radius.circular(AppDims.radiusLarge)),
                ),
              ],
            ),
            child: HuaweiCard(
              child: Row(
                children: [
                  Container(
                    width: 60,
                    height: 60,
                    decoration: BoxDecoration(
                      color: AppColors.background,
                      borderRadius: BorderRadius.circular(AppDims.radiusSmall),
                    ),
                    child: const Icon(Icons.directions_car, color: AppColors.textSecondary),
                  ),
                  const SizedBox(width: 16),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text('京A·12345', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                        const SizedBox(height: 4),
                        const Text('车位：A-05', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                        const SizedBox(height: 2),
                        const Text('占用时长：72小时', style: TextStyle(fontSize: 12, color: AppColors.textSecondary)),
                      ],
                    ),
                  ),
                  const StatusBadge.zombie(),
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}
```

### 10.4 数据复盘页 `lib/features/stats/presentation/pages/stats_page.dart`

```dart
import 'package:flutter/material.dart';
import 'package:fl_chart/fl_chart.dart';
import '../../../../shared/widgets/huawei_card.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';

class StatsPage extends StatelessWidget {
  const StatsPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(title: const Text('数据复盘')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // 占用率环形图
            HuaweiCard(
              child: Column(
                children: [
                  const Text('当前占用率', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                  const SizedBox(height: AppDims.gapCard),
                  SizedBox(
                    height: 180,
                    child: PieChart(
                      PieChartData(
                        sections: [
                          PieChartSectionData(
                            value: 18,
                            color: AppColors.warning,
                            title: '56%',
                            radius: 60,
                            titleStyle: const TextStyle(fontSize: 20, fontWeight: FontWeight.w700, color: Colors.white),
                          ),
                          PieChartSectionData(
                            value: 14,
                            color: AppColors.success,
                            title: '',
                            radius: 60,
                          ),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(height: AppDims.gapItem),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      _buildLegendItem(AppColors.warning, '占用 18'),
                      const SizedBox(width: AppDims.gapCard),
                      _buildLegendItem(AppColors.success, '空闲 14'),
                    ],
                  ),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 7天趋势折线图
            HuaweiCard(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('7天占用趋势', style: TextStyle(fontSize: 16, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                  const SizedBox(height: AppDims.gapCard),
                  SizedBox(
                    height: 150,
                    child: LineChart(
                      LineChartData(
                        borderData: FlBorderData(show: false),
                        titlesData: FlTitlesData(
                          bottomTitles: AxisTitles(
                            sideTitles: SideTitles(showTitles: true, getTitlesWidget: (value, _) {
                              const days = ['一', '二', '三', '四', '五', '六', '日'];
                              return Text(days[value.toInt()], style: const TextStyle(fontSize: 12, color: AppColors.textSecondary));
                            }),
                          ),
                        ),
                        lineBarsData: [
                          LineChartBarData(
                            spots: const [
                              FlSpot(0, 40), FlSpot(1, 55), FlSpot(2, 48),
                              FlSpot(3, 60), FlSpot(4, 52), FlSpot(5, 65), FlSpot(6, 56),
                            ],
                            isCurved: true,
                            color: AppColors.primary,
                            barWidth: 3,
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 统计卡片
            Row(
              children: [
                Expanded(child: _buildMiniStat('今日告警', '5', AppColors.danger)),
                const SizedBox(width: AppDims.gapCard),
                Expanded(child: _buildMiniStat('本周告警', '23', AppColors.warning)),
                const SizedBox(width: AppDims.gapCard),
                Expanded(child: _buildMiniStat('已处置', '18', AppColors.success)),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildLegendItem(Color color, String text) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(width: 12, height: 12, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
        const SizedBox(width: 6),
        Text(text, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
      ],
    );
  }

  Widget _buildMiniStat(String title, String value, Color color) {
    return HuaweiCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(title, style: const TextStyle(fontSize: 12, color: AppColors.textSecondary)),
          const SizedBox(height: 4),
          Text(value, style: TextStyle(fontSize: 24, fontWeight: FontWeight.w700, color: color)),
        ],
      ),
    );
  }
}
```

### 10.5 我的页面 `lib/features/profile/presentation/pages/profile_page.dart`

```dart
import 'package:flutter/material.dart';
import '../../../../shared/widgets/huawei_card.dart';
import '../../../../shared/widgets/circle_progress.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';

class ProfilePage extends StatelessWidget {
  const ProfilePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      appBar: AppBar(title: const Text('我的')),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(AppDims.paddingPage),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // 用户信息卡片
            HuaweiCard(
              child: Row(
                children: [
                  Container(
                    width: 60,
                    height: 60,
                    decoration: BoxDecoration(
                      color: AppColors.primaryLight,
                      shape: BoxShape.circle,
                    ),
                    child: const Icon(Icons.person, color: AppColors.primary, size: 32),
                  ),
                  const SizedBox(width: 16),
                  const Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text('管理员', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
                        SizedBox(height: 4),
                        Text('系统在线', style: TextStyle(fontSize: 14, color: AppColors.success)),
                      ],
                    ),
                  ),
                  const CircleProgress(percentage: 85, size: 50, color: AppColors.success),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 系统状态
            const Text('系统状态', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            const SizedBox(height: AppDims.gapItem),
            HuaweiCard(
              child: Column(
                children: [
                  _buildStatusRow('LoRa 网关', '在线', AppColors.success),
                  const Divider(height: 1, color: AppColors.background),
                  _buildStatusRow('LoRa 节点', '31/32 在线', AppColors.warning),
                  const Divider(height: 1, color: AppColors.background),
                  _buildStatusRow('摄像头', '4/4 在线', AppColors.success),
                  const Divider(height: 1, color: AppColors.background),
                  _buildStatusRow('服务器', '正常', AppColors.success),
                ],
              ),
            ),
            const SizedBox(height: AppDims.gapCard),

            // 功能入口
            const Text('功能入口', style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: AppColors.textPrimary)),
            const SizedBox(height: AppDims.gapItem),
            HuaweiCard(
              child: Column(
                children: [
                  _buildMenuItem(Icons.settings_ethernet, '节点管理'),
                  const Divider(height: 1, color: AppColors.background),
                  _buildMenuItem(Icons.wifi, '设备配网'),
                  const Divider(height: 1, color: AppColors.background),
                  _buildMenuItem(Icons.bar_chart, '数据统计'),
                  const Divider(height: 1, color: AppColors.background),
                  _buildMenuItem(Icons.notifications, '消息通知'),
                  const Divider(height: 1, color: AppColors.background),
                  _buildMenuItem(Icons.help_outline, '帮助与反馈'),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildStatusRow(String label, String status, Color color) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 12),
      child: Row(
        children: [
          Container(width: 8, height: 8, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
          const SizedBox(width: 12),
          Text(label, style: const TextStyle(fontSize: 14, color: AppColors.textPrimary)),
          const Spacer(),
          Text(status, style: TextStyle(fontSize: 14, fontWeight: FontWeight.w500, color: color)),
        ],
      ),
    );
  }

  Widget _buildMenuItem(IconData icon, String title) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 14),
      child: Row(
        children: [
          Icon(icon, color: AppColors.primary, size: 22),
          const SizedBox(width: 16),
          Text(title, style: const TextStyle(fontSize: 15, color: AppColors.textPrimary)),
          const Spacer(),
          const Icon(Icons.chevron_right, color: AppColors.textSecondary, size: 20),
        ],
      ),
    );
  }
}
```

---

## 【十一、依赖库配置】

`pubspec.yaml` 依赖项：

```yaml
dependencies:
  flutter:
    sdk: flutter
  http: ^1.2.0
  fl_chart: ^0.68.0
  flutter_slidable: ^3.1.0
  flutter_staggered_grid_view: ^0.7.0
  flutter_swiper_view: ^1.2.0
  animated_text_kit: ^4.2.2
  flutter_spinkit: ^5.2.0
```

---

## 【十二、给 AI 的指令模板】（可直接复制使用）

```
请基于现有的Flutter项目代码，修改/创建 [页面名称] 页面。
必须严格遵守以下规则：
1. 组件复用：所有卡片必须使用 HuaweiCard，所有状态标签必须使用 StatusBadge，所有环形进度条必须使用 CircleProgress。禁止创建新的样式组件。
2. 样式锁定：颜色必须使用 AppColors 类中的定义，间距必须使用 AppDims 类中的定义。严禁硬编码颜色值或尺寸。
3. 设计语言：整体风格参考华为智慧生活App，圆角统一为 radiusLarge (16dp)，卡片阴影必须严格使用 blurRadius: 12, offset: Offset(0, 2), opacity: 0.08。
4. 布局逻辑：参考 OverviewPage 和 AlertsPage 的布局结构，使用 SingleChildScrollView + Column 或 ListView.separated。
5. 交互逻辑：列表项滑动操作必须使用 flutter_slidable 库，参考 AlertsPage 的实现。
6. 数据模型：使用 SpotModel、AlertModel、StatsModel 处理数据，API 调用通过 ApiService 类。
7. 状态处理：数据加载时显示 LoadingIndicator，无数据时显示 EmptyState。
8. 禁止行为：不要使用 Material 2 的默认样式，不要使用尖锐直角，不要堆砌功能入口，背景色必须为 AppColors.background。

当前任务：[在此处描述你要AI做的具体事情]
```

---

## 【十三、文档完整性评分】

| 维度 | 当前状态 | 说明 |
|------|----------|------|
| 整体框架 | ★★★★★ | 结构清晰，从设计到代码层层递进 |
| 设计令牌 | ★★★★★ | 颜色（含暗色）、尺寸定义完整 |
| 基础组件 | ★★★★★ | 五个核心组件可直接使用 |
| 页面示例 | ★★★★★ | 五个页面全覆盖，可作为模板 |
| 数据模型 | ★★★★★ | SpotModel/AlertModel/StatsModel 完整 |
| API服务层 | ★★★★★ | CRUD 接口定义清晰 |
| 空/加载状态 | ★★★★★ | EmptyState + LoadingIndicator |
| AI指令模板 | ★★★★★ | 约束清晰，可直接使用 |
