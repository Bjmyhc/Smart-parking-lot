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
- 留白：页面边距 20dp，卡片间距 12dp（华为改版强调"适当增加留白"）
- 导航栏：大标题风格（28sp 粗体），右侧通知图标
- 底部导航：3 个 Tab（总览/车位/我的），选中态蓝色高亮 + 图标放大

---

## 【二、组件规范】

### 2.1 核心组件清单

| 组件 | 文件 | 用途 |
|------|------|------|
| CustomAppBar | `custom_app_bar.dart` | 蓝色渐变导航栏，支持返回/操作按钮 |
| CardContainer | `card_container.dart` | 白色卡片容器，16dp 圆角 + 柔和阴影 |
| ListTileBase | `list_tile_base.dart` | 列表项基类，左图标+中标题+右箭头 |
| ActionButtonGroup | `action_button_group.dart` | 三按钮组：派单(蓝)/通知(橙)/处置(绿) |
| StatCard | `stat_card.dart` | 统计卡片，含图标+数值+趋势标签 |
| StatusBadge | `status_badge.dart` | 状态徽章，支持 factory 构造 |
| CircleProgress | `circle_progress.dart` | 圆环进度条 |
| HuaweiCard | `huawei_card.dart` | 华为风格卡片（预留，优先用 CardContainer） |
| EmptyState | `empty_state.dart` | 空状态占位 |
| LoadingIndicator | `loading_indicator.dart` | 加载动画（SpinKitFadingCircle） |

### 2.2 组件设计规范

- 车位状态卡片：使用 `CardContainer` + `StatusBadge`，左侧车位编号大字号，
  右侧三色状态徽章（空闲/占用/僵尸车）
- 设备卡片：参考华为智慧生活，展示 LoRa 节点图标、在线状态、
  电池电量环形进度、信号强度
- 列表项：使用 `ListTileBase`，左图标 + 中标题/副标题 + 右箭头或状态标签，
  分割线用 0.5dp 浅灰（opacity 0.35）
- 详情页：顶部车牌识别结果，中部信息分区卡片，底部 `ActionButtonGroup`
- 导航：底部 TabBar 3 个（总览/车位/我的），选中色用华为蓝
- 图表：占用率用环形图，7天趋势用折线图，使用 fl_chart 库

### 2.3 组件优先级

1. **优先使用**：`CardContainer`（通用卡片）、`ListTileBase`（列表项）、`StatusBadge`（状态标签）
2. **按需使用**：`StatCard`（KPI 数据卡）、`ActionButtonGroup`（三按钮操作）、`CustomAppBar`（蓝色渐变导航）
3. **预留使用**：`HuaweiCard`（已被 CardContainer 替代，保留兼容性）

---

## 【三、页面结构】

### 3.1 页面总览

1. **总览页 (OverviewPage)**
   - 大标题 "总览" + 右侧通知图标
   - 问候卡片（时间问候 + 日期 + 系统健康状态）
   - 车位概览卡片（总车位/空闲/占用/僵尸车 四格分布）
   - 车位实时状态圆环图（空闲/占用/僵尸车占比 + 图例 + 区域建议）
   - 最新僵尸车告警卡片

2. **车位页 (SpotsPage)**
   - 大标题 "车位" + 右侧筛选图标
   - 区域筛选（A区/B区/C区）SegmentedControl
   - 车位网格卡片（状态徽章 + 编号 + 占用信息）
   - 点击进入车位详情页

3. **车位详情页 (SpotDetailPage)**
   - CustomAppBar 蓝色渐变 + 返回按钮
   - 车牌大图卡片
   - 车辆信息卡片（车位编号/分区/入场时间/信号/电量）
   - ActionButtonGroup（派单/通知车主/标记已处置）

4. **预警页 (AlertsPage)**
   - CustomAppBar 蓝色渐变
   - 僵尸车工单列表（车牌号/车位/占用时长/状态标签）
   - flutter_slidable 滑动操作（左滑派单/通知/处置）

5. **数据分析页 (StatsPage)** — 规划中
   - 占用率环形图 + 7天趋势折线图
   - 告警统计卡片

6. **我的页 (ProfilePage)**
   - 大标题 "我的" + 右侧通知图标
   - 用户卡片（头像 + 管理员信息 + 三项统计）
   - 快捷操作区（告警中心/车位管理/数据导出/云端同步）
   - 功能分组卡片（系统设置/节点管理/策略配置）
   - 设备分组卡片（操作日志/固件升级/故障诊断）
   - 账号分组卡片（个人资料/账号安全/帮助反馈/关于）

### 3.2 导航结构

```
MaterialApp
  └── MainShell (Scaffold)
       ├── body: IndexedStack
       │   ├── OverviewPage (index 0)
       │   ├── SpotsPage (index 1)
       │   └── ProfilePage (index 2)
       └── bottomNavigationBar: 自定义 Row 导航
           ├── 总览 (dashboard_outlined → dashboard)
           ├── 车位 (local_parking_outlined → local_parking)
           └── 我的 (person_outline → person)

  路由:
  ├── /alerts → AlertsPage
  └── (push) → SpotDetailPage(spotId)
```

---

## 【四、推荐的 Flutter UI 库】

| 库名 | 用途 | 版本 | 说明 |
|------|------|------|------|
| fl_chart | 图表 | ^0.68.0 | 环形图、折线图、柱状图 |
| flutter_slidable | 滑动操作 | ^3.1.0 | 工单列表项左滑操作 |
| flutter_staggered_grid_view | 网格布局 | ^0.7.0 | 车位状态网格 |
| flutter_swiper_view | 轮播 | ^1.1.8 | 车位图片轮播、引导页 |
| animated_text_kit | 文字动画 | ^4.2.2 | 数据加载、数字滚动效果 |
| flutter_spinkit | 加载指示器 | ^5.2.0 | 拍照识别时的 loading 动画 |
| crypto | 加密 | ^3.0.3 | HMAC-MD5 签名 |

> **注意**：本项目已手写 `CardContainer`、`StatusBadge`、`StatCard`、`ListTileBase`、`ActionButtonGroup` 等基础组件，优先使用手写组件。

---

## 【五、禁止事项】

- 不要用 Material 2 的默认深紫粉色主题
- 不要用尖锐直角按钮
- 不要堆砌功能入口（华为的教训：设备多时会拥挤）
- 不要用刺眼的纯白色背景
- 列表项之间要有充足留白，不要紧凑排列
- 不要硬编码颜色值，统一使用 `AppColors`
- 不要硬编码尺寸值，统一使用 `AppDims`
- 卡片内边距默认 16dp，不要随意修改
- 分割线使用 0.5dp + 35% 透明度，不要用实线深色

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
  static const double gapCard = 12.0;
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

  // 便捷 getter
  bool get isFree => status == 'free';
  bool get isOccupied => status == 'occupied';
  bool get isZombie => status == 'zombie';

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
  static Future<List<SpotModel>> getSpots() async { ... }

  /// 获取告警列表
  static Future<List<AlertModel>> getAlerts() async { ... }

  /// 派单
  static Future<bool> dispatchAlert(String alertId) async { ... }

  /// 通知车主
  static Future<bool> notifyOwner(String alertId) async { ... }

  /// 标记已处置
  static Future<bool> resolveAlert(String alertId) async { ... }

  /// 获取统计数据
  static Future<StatsModel> getStats() async { ... }
}
```

---

## 【九、基础组件实现】

### 9.1 卡片容器 `lib/shared/widgets/card_container.dart`

```dart
class CardContainer extends StatelessWidget {
  final Widget child;
  final EdgeInsetsGeometry? padding;
  final VoidCallback? onTap;
  final Color? backgroundColor;

  // 默认: padding=16dp, 圆角=16dp, 柔和阴影
  // onTap 为 null 时纯展示，有值时包裹 GestureDetector
}
```

### 9.2 自定义导航栏 `lib/shared/widgets/custom_app_bar.dart`

```dart
class CustomAppBar extends StatelessWidget implements PreferredSizeWidget {
  final String title;
  final bool showBackButton;
  final List<Widget>? actions;
  final Widget? leading;
  final PreferredSizeWidget? bottom;

  // 蓝色渐变背景: primary → Color(0xFF4A9EFF)
  // 白色标题文字 + 白色图标
}
```

### 9.3 列表项基类 `lib/shared/widgets/list_tile_base.dart`

```dart
class ListTileBase extends StatelessWidget {
  final Widget? leading;    // 左侧图标
  final Widget title;       // 主标题
  final Widget? subtitle;   // 副标题
  final Widget? trailing;   // 右侧内容
  final VoidCallback? onTap;

  // 默认: vertical 12 + horizontal 16 边距
  // 点击态: InkWell + 12dp 圆角
}
```

### 9.4 操作按钮组 `lib/shared/widgets/action_button_group.dart`

```dart
class ActionButtonGroup extends StatelessWidget {
  final VoidCallback? onDispatch;  // 派单 - 蓝色
  final VoidCallback? onNotify;    // 通知 - 橙色
  final VoidCallback? onResolve;   // 处置 - 绿色

  // 三按钮横排，每个按钮: 图标 + 文字
  // 背景色为对应主色的 10% 透明度
}
```

### 9.5 统计卡片 `lib/shared/widgets/stat_card.dart`

```dart
class StatCard extends StatelessWidget {
  final String title;
  final String value;
  final String? trend;          // 趋势文字，如 "+12%"
  final bool trendIsUp;        // true=上升(绿), false=下降(红)
  final IconData icon;
  final Color color;
  final VoidCallback? onTap;

  // 结构: 图标 + 趋势标签 | 数值 | 标题
}
```

### 9.6 状态徽章 `lib/shared/widgets/status_badge.dart`

```dart
class StatusBadge extends StatelessWidget {
  // Factory 构造: .free() / .occupied() / .zombie()
  // 也支持 .fromStatus(status) 根据字符串自动匹配
  // 样式: 10% 透明度背景 + 30% 透明度边框 + 对应主色文字
}
```

### 9.7 环形进度条 `lib/shared/widgets/circle_progress.dart`

```dart
class CircleProgress extends StatelessWidget {
  final double percentage;  // 0-100
  final double size;        // 默认 40
  final Color color;
}
```

### 9.8 空状态 `lib/shared/widgets/empty_state.dart`

```dart
class EmptyState extends StatelessWidget {
  final String message;
  // 居中显示图标 + 文字
}
```

### 9.9 加载指示器 `lib/shared/widgets/loading_indicator.dart`

```dart
class LoadingIndicator extends StatelessWidget {
  final double size;  // 默认 50
  // SpinKitFadingCircle 动画
}
```

---

## 【十、页面实现示例】

### 10.1 总览页结构 `lib/features/overview/presentation/pages/overview_page.dart`

```dart
// 页面结构（自上而下）:
// ├── _buildHeader()      // 大标题"总览" + 通知图标
// ├── _buildGreetingCard() // 问候卡片（时间问候 + 系统健康）
// ├── _buildStatsCard()    // 车位概览（总数/空闲/占用/僵尸车）
// ├── _buildChartCard()    // 圆环图 + 图例 + 区域建议
// └── _buildLatestAlertCard() // 最新僵尸车告警

// 数据来源: ApiService.getSpots() + getAlerts()
// 刷新: RefreshIndicator 下拉刷新
// 加载: CircularProgressIndicator
```

### 10.2 车位页结构 `lib/features/spots/presentation/pages/spots_page.dart`

```dart
// 页面结构:
// ├── 大标题"车位" + 筛选图标
// ├── 区域筛选 SegmentedControl (A/B/C)
// └── 车位网格卡片列表
//     ├── StatusBadge (free/occupied/zombie)
//     ├── 车位编号
//     └── 占用时长/入场时间

// 点击 → Navigator.push → SpotDetailPage
```

### 10.3 车位详情页结构 `lib/features/spots/presentation/pages/spot_detail_page.dart`

```dart
// 使用 CustomAppBar (蓝色渐变 + 返回按钮)
// ├── 车牌大图卡片 (CardContainer)
// ├── 车辆信息卡片 (CardContainer + 分割线)
// │   ├── 车位编号
// │   ├── 所在分区
// │   ├── 入场时间
// │   ├── LoRa 信号
// │   └── 电池电量
// └── ActionButtonGroup (派单/通知/处置)
```

### 10.4 预警页结构 `lib/features/alerts/presentation/pages/alerts_page.dart`

```dart
// 使用 CustomAppBar (蓝色渐变)
// └── ListView.separated
//     └── Slidable (flutter_slidable)
//         ├── 左滑操作: 派单(蓝) / 通知(橙) / 处置(绿)
//         └── CardContainer 内容
//             ├── 车辆图标
//             ├── 车牌号 + 车位 + 占用时长
//             └── StatusBadge
```

### 10.5 我的页结构 `lib/features/profile/presentation/pages/profile_page.dart`

```dart
// 页面结构（自上而下）:
// ├── 大标题"我的" + 通知图标
// ├── _buildUserCard()      // 头像 + 管理员信息 + 三项统计
// ├── _buildQuickActions()  // 快捷操作（4 个图标按钮）
// ├── _buildSystemGroup()   // 系统设置/节点管理/策略配置
// ├── _buildDeviceGroup()   // 操作日志/固件升级/故障诊断
// └── _buildAccountGroup()  // 个人资料/账号安全/帮助/关于

// 分组卡片: CardContainer + _buildDivider() (0.5dp, opacity 0.35)
// 列表项: 自定义 _buildGroupItem (图标 + 标题 + 箭头)
```

---

## 【十一、依赖库配置】

`pubspec.yaml` 依赖项：

```yaml
dependencies:
  flutter:
    sdk: flutter
  http: ^1.2.0
  crypto: ^3.0.3
  fl_chart: ^0.68.0
  flutter_slidable: ^3.1.0
  flutter_staggered_grid_view: ^0.7.0
  flutter_swiper_view: ^1.1.8
  animated_text_kit: ^4.2.2
  flutter_spinkit: ^5.2.0
```

---

## 【十二、给 AI 的指令模板】（可直接复制使用）

```
请基于现有的Flutter项目代码，修改/创建 [页面名称] 页面。
必须严格遵守以下规则：
1. 组件复用：所有卡片必须使用 CardContainer，所有状态标签必须使用 StatusBadge，
   所有列表项必须使用 ListTileBase，所有多按钮操作必须使用 ActionButtonGroup。
   禁止创建新的样式组件。
2. 样式锁定：颜色必须使用 AppColors 类中的定义，间距必须使用 AppDims 类中的定义。
   严禁硬编码颜色值或尺寸。
3. 设计语言：整体风格参考华为智慧生活App，大标题风格（28sp w700），
   卡片圆角统一为 radiusLarge (16dp)，卡片阴影必须严格使用
   blurRadius: 12, offset: Offset(0, 2), opacity: 0.08。
4. 布局逻辑：参考 OverviewPage 和 ProfilePage 的布局结构，
   使用 SingleChildScrollView + Column 或 ListView.separated。
5. 交互逻辑：列表项滑动操作必须使用 flutter_slidable 库，参考 AlertsPage 的实现。
6. 数据模型：使用 SpotModel、AlertModel、StatsModel 处理数据，
   API 调用通过 ApiService 类。
7. 状态处理：数据加载时显示 LoadingIndicator，无数据时显示 EmptyState。
8. 导航规范：子页面（详情页/预警页）使用 CustomAppBar 蓝色渐变导航栏，
   主页使用大标题风格（Text 28sp w700 + 通知图标）。
9. 禁止行为：不要使用 Material 2 的默认样式，不要使用尖锐直角，
   不要堆砌功能入口，背景色必须为 AppColors.background。
10. 分割线规范：使用 Container(height: 1, color: AppColors.textSecondary.withOpacity(0.35))，
    不要使用 Flutter 的 Divider widget（间距太大）。

当前任务：[在此处描述你要AI做的具体事情]
```

---

## 【十三、文档完整性评分】

| 维度 | 当前状态 | 说明 |
|------|----------|------|
| 整体框架 | ★★★★★ | 结构清晰，大标题风格 + 白色卡片分组 |
| 设计令牌 | ★★★★★ | 颜色（含暗色）、尺寸定义完整 |
| 基础组件 | ★★★★★ | 10 个组件可直接使用，含优先级说明 |
| 页面示例 | ★★★★☆ | 5 个核心页面，数据分析页待完善 |
| 数据模型 | ★★★★★ | SpotModel/AlertModel/StatsModel 完整 |
| API服务层 | ★★★★★ | CRUD 接口定义清晰 |
| 空/加载状态 | ★★★★★ | EmptyState + LoadingIndicator |
| AI指令模板 | ★★★★★ | 约束清晰，10 条规则 |
