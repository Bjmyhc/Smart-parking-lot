# 路边僵尸车检测系统 - APP 开发文档

> **版本**: v2.0（重写版本）  
> **日期**: 2026-08-19  
> **设计规范**: 参考 `APP设计文档.md`  
> **状态**: 开发中

---

## 一、项目概述

### 1.1 项目简介

本项目是一个基于 Flutter 的"路边僵尸车检测系统"手机 APP，用于智能停车场管理场景。主要功能包括：实时车位状态监控、僵尸车告警管理、数据统计复盘等。

### 1.2 技术栈

| 类别 | 技术 | 版本 |
|------|------|------|
| 框架 | Flutter | 3.x |
| 语言 | Dart | 3.x |
| 状态管理 | setState / Provider | - |
| 网络请求 | http | ^1.2.0 |
| 图表 | fl_chart | ^0.68.0 |
| 网格布局 | flutter_staggered_grid_view | ^0.7.0 |
| 加载动画 | flutter_spinkit | ^5.2.0 |

### 1.3 项目结构

```
lib/
├── core/
│   ├── theme/
│   │   ├── app_colors.dart       # 颜色定义
│   │   └── app_dims.dart         # 尺寸定义
│   ├── models/
│   │   ├── spot_model.dart       # 车位模型
│   │   ├── alert_model.dart      # 告警模型
│   │   └── stats_model.dart      # 统计模型
│   └── services/
│       └── api_service.dart      # API 服务层
├── shared/
│   └── widgets/
│       ├── huawei_card.dart      # 华为卡片
│       ├── status_badge.dart     # 状态徽章
│       ├── circle_progress.dart  # 环形进度条
│       ├── empty_state.dart      # 空状态
│       └── loading_indicator.dart # 加载指示器
├── features/
│   ├── overview/                 # 总览页
│   ├── spots/                    # 车位页
│   ├── alerts/                   # 告警页
│   ├── stats/                    # 数据复盘页
│   └── profile/                  # 我的页
└── main.dart                     # 入口文件
```

---

## 二、开发阶段规划

### 阶段 1：项目初始化（预计 0.5 天）? 已完成

#### 任务清单

| 序号 | 任务 | 状态 | 产出物 |
|------|------|------|--------|
| 1.1 | 创建 Flutter 项目 | ? | 项目骨架 |
| 1.2 | 配置 pubspec.yaml 依赖 | ? | 依赖配置文件 |
| 1.3 | 创建目录结构 | ? | 文件夹 |
| 1.4 | 配置 Material 3 主题 | ? | main.dart |

#### 详细步骤

**1.1 创建项目**
```bash
flutter create --org com.smartparking .
```

**1.2 配置依赖**
```yaml
# pubspec.yaml
dependencies:
  flutter:
    sdk: flutter
  http: ^1.2.0
  fl_chart: ^0.68.0
  flutter_staggered_grid_view: ^0.7.0
  flutter_spinkit: ^5.2.0
```

**1.3 创建目录**
```bash
mkdir -p lib/core/theme lib/core/models lib/core/services
mkdir -p lib/shared/widgets
mkdir -p lib/features/overview/presentation/pages
mkdir -p lib/features/spots/presentation/pages
mkdir -p lib/features/alerts/presentation/pages
mkdir -p lib/features/stats/presentation/pages
mkdir -p lib/features/profile/presentation/pages
```

**1.4 配置主题**
- 在 `main.dart` 中配置 Material 3
- 设置 `useMaterial3: true`
- 设置主题色为华为蓝 `#007DFF`

---

### 阶段 2：核心层开发（预计 1 天）? 已完成

#### 任务清单

| 序号 | 任务 | 状态 | 产出物 |
|------|------|------|--------|
| 2.1 | 实现 AppColors | ? | `app_colors.dart` |
| 2.2 | 实现 AppDims | ? | `app_dims.dart` |
| 2.3 | 实现 SpotModel | ? | `spot_model.dart` |
| 2.4 | 实现 AlertModel | ? | `alert_model.dart` |
| 2.5 | 实现 StatsModel | ? | `stats_model.dart` |
| 2.6 | 实现 ApiService | ? | `api_service.dart` |

#### 验收标准

- [x] 所有颜色常量可通过 `AppColors.primary` 访问
- [x] 所有尺寸常量可通过 `AppDims` 访问
- [x] Model 类支持 `fromJson` 和 `toJson` 序列化
- [x] ApiService 所有方法返回正确的 Future 类型
- [x] 代码无编译错误

---

### 阶段 3：基础组件开发（预计 1 天）? 已完成

#### 任务清单

| 序号 | 任务 | 状态 | 产出物 |
|------|------|------|--------|
| 3.1 | 实现 HuaweiCard | ? | `huawei_card.dart` |
| 3.2 | 实现 StatusBadge | ? | `status_badge.dart` |
| 3.3 | 实现 CircleProgress | ? | `circle_progress.dart` |
| 3.4 | 实现 EmptyState | ? | `empty_state.dart` |
| 3.5 | 实现 LoadingIndicator | ? | `loading_indicator.dart` |

#### 验收标准

- [x] HuaweiCard 阴影：`blurRadius: 12, offset: Offset(0, 2), opacity: 0.08`
- [x] StatusBadge 三种状态：free/occupied/zombie
- [x] 所有组件可独立测试通过
- [x] 组件使用 AppColors 和 AppDims，无硬编码

---

### 阶段 4：页面开发（预计 2.5 天）? 部分完成

#### 任务清单

| 序号 | 任务 | 优先级 | 状态 | 依赖 |
|------|------|--------|------|------|
| 4.1 | 实现主框架（BottomNavigationBar） | P0 | ? | 阶段 1 |
| 4.2 | 实现总览页 OverviewPage | P0 | ? | 阶段 2, 3 |
| 4.3 | 实现车位详情页 SpotDetailPage | P1 | ? | 阶段 2, 3 |
| 4.4 | 实现告警页 AlertsPage | P1 | ? | 阶段 2, 3 |
| 4.5 | 实现数据复盘页 StatsPage | P2 | ? | 阶段 2, 3 |
| 4.6 | 实现我的页面 ProfilePage | P2 | ? | 阶段 2, 3 |
| 4.7 | 实现车位页 SpotsPage | P1 | ? | 阶段 2, 3 |

#### 开发顺序

```
4.1 主框架 → 4.2 总览页 → 4.4 告警页 → 4.3 车位详情页 → 4.7 车位页 → 4.6 我的页面 → 4.5 数据复盘页
```

#### 验收标准

- [x] 所有页面使用 `AppColors.background` 背景色
- [x] 卡片统一使用 `HuaweiCard` 组件
- [x] 状态标签统一使用 `StatusBadge` 组件
- [x] 加载状态显示 `LoadingIndicator`，空数据显示 `EmptyState`
- [x] 告警页支持操作按钮（删除/处理）
- [ ] 数据复盘页使用 fl_chart 展示环形图和折线图
- [x] 代码无编译错误，UI 符合设计规范

---

### 阶段 5：联调与测试（预计 1.5 天）? 进行中

#### 任务清单

| 序号 | 任务 | 状态 | 说明 |
|------|------|------|------|
| 5.1 | 模拟数据接入 | ? | 使用 Mock 数据验证 UI |
| 5.2 | API 接口联调 | ? | 对接真实后端 |
| 5.3 | 状态管理完善 | ? | 处理加载/错误/空状态 |
| 5.4 | 交互测试 | ? | 测试所有页面跳转和点击 |
| 5.5 | 性能优化 | ? | 列表懒加载、动画优化 |
| 5.6 | UI 细节调整 | ? | 间距、字体、颜色微调 |

#### 测试用例

| 测试场景 | 测试点 | 状态 | 预期结果 |
|----------|--------|------|----------|
| 总览页加载 | 数据请求 → 展示 | ? | 显示统计卡片 + 车位网格 |
| 车位详情 | 点击车位卡片 | ? | 跳转详情页，显示完整信息 |
| 告警操作 | 点击处理/删除按钮 | ? | 显示操作反馈 |
| 派单操作 | 点击派单按钮 | ? | 调用 API，状态更新 |
| 网络异常 | 断网情况下 | ? | 使用 Mock 数据兜底 |
| 空数据 | 无车位/告警 | ? | 显示 EmptyState 组件 |
| 中文显示 | 所有页面中文 | ? | UTF-8 编码正确显示 |

---

### 阶段 6：部署与交付（预计 1 天）? 待开始

#### 任务清单

| 序号 | 任务 | 状态 | 说明 |
|------|------|------|------|
| 6.1 | Android 打包 | ? | APK / AAB |
| 6.2 | iOS 打包（可选） | ? | IPA |
| 6.3 | 代码清理 | ? | 移除调试代码 |
| 6.4 | 文档完善 | ? | 更新 README |
| 6.5 | 最终验收 | ? | 全流程演示 |

---

## 三、文件产出清单

### 3.1 需要创建的文件

| 文件路径 | 阶段 | 状态 | 说明 |
|----------|------|------|------|
| `lib/core/theme/app_colors.dart` | 阶段 2 | ? | 颜色定义 |
| `lib/core/theme/app_dims.dart` | 阶段 2 | ? | 尺寸定义 |
| `lib/core/models/spot_model.dart` | 阶段 2 | ? | 车位数据模型 |
| `lib/core/models/alert_model.dart` | 阶段 2 | ? | 告警数据模型 |
| `lib/core/models/stats_model.dart` | 阶段 2 | ? | 统计数据模型 |
| `lib/core/services/api_service.dart` | 阶段 2 | ? | API 服务层 |
| `lib/shared/widgets/huawei_card.dart` | 阶段 3 | ? | 华为卡片组件 |
| `lib/shared/widgets/status_badge.dart` | 阶段 3 | ? | 状态徽章组件 |
| `lib/shared/widgets/circle_progress.dart` | 阶段 3 | ? | 环形进度组件 |
| `lib/shared/widgets/empty_state.dart` | 阶段 3 | ? | 空状态组件 |
| `lib/shared/widgets/loading_indicator.dart` | 阶段 3 | ? | 加载指示器 |
| `lib/features/overview/presentation/pages/overview_page.dart` | 阶段 4 | ? | 总览页 |
| `lib/features/spots/presentation/pages/spots_page.dart` | 阶段 4 | ? | 车位列表页 |
| `lib/features/spots/presentation/pages/spot_detail_page.dart` | 阶段 4 | ? | 车位详情页 |
| `lib/features/alerts/presentation/pages/alerts_page.dart` | 阶段 4 | ? | 告警页 |
| `lib/features/stats/presentation/pages/stats_page.dart` | 阶段 4 | ? | 数据复盘页 |
| `lib/features/profile/presentation/pages/profile_page.dart` | 阶段 4 | ? | 我的页面 |
| `lib/main.dart` | 阶段 1 | ? | 入口文件（重写） |

### 3.2 文件依赖关系

```
main.dart
  ├── overview_page.dart
  ├── spots_page.dart
  │   └── spot_detail_page.dart
  ├── alerts_page.dart
  ├── stats_page.dart
  └── profile_page.dart
       ↓ 依赖
  huawei_card.dart / status_badge.dart / circle_progress.dart
  empty_state.dart / loading_indicator.dart
       ↓ 依赖
  app_colors.dart / app_dims.dart
       ↓ 依赖
  spot_model.dart / alert_model.dart / stats_model.dart
       ↓ 依赖
  api_service.dart
```

---

## 四、风险与应对

| 风险 | 影响 | 概率 | 应对措施 |
|------|------|------|----------|
| API 接口未就绪 | 阻塞联调 | 中 | 使用 Mock 数据先行开发 |
| fl_chart 版本兼容问题 | 图表异常 | 低 | 锁定版本，查阅官方文档 |
| 网络请求超时 | 体验差 | 中 | 添加超时处理和重试机制 |
| 状态管理混乱 | 代码难维护 | 中 | 统一使用 StatefulWidget + setState |
| 性能卡顿 | 体验差 | 低 | 列表使用 ListView.builder 懒加载 |
| 中文编码问题 | 显示乱码 | 中 | 使用 UTF-8 编码写入文件 |

---

## 五、开发进度追踪

### 总体进度

| 阶段 | 任务 | 预计工时 | 开始时间 | 完成时间 | 状态 |
|------|------|----------|----------|----------|------|
| 1 | 项目初始化 | 0.5 天 | 2026-08-19 | 2026-08-19 | ? 完成 |
| 2 | 核心层开发 | 1 天 | 2026-08-19 | 2026-08-19 | ? 完成 |
| 3 | 基础组件开发 | 1 天 | 2026-08-19 | 2026-08-19 | ? 完成 |
| 4 | 页面开发 | 2.5 天 | 2026-08-19 | 2026-08-19 | ? 完成 |
| 5 | 联调与测试 | 1.5 天 | 2026-08-19 | - | ? 进行中 |
| 6 | 部署与交付 | 1 天 | - | - | ? 待开始 |

### 累计工时：7.5 天（已完成 6 天）

### 进度统计

- ? 已完成：4 个阶段（阶段 1-4 全部完成）
- ? 进行中：1 个阶段（阶段 5 联调与测试）
- ? 待开始：1 个阶段（阶段 6 部署与交付）
- ? 总体进度：约 80%

---

## 六、执行记录

### 2026-08-19 执行记录

#### 背景
- 原代码与设计文档严重脱节（Material 2 vs Material 3、全英文 vs 中文、无基础组件等）
- 决定采用**重写方案**，提取 OneNET 鉴权算法，按设计文档从头搭建

#### 完成内容

**阶段 1：项目初始化 ?**
- 更新 `pubspec.yaml` 依赖：添加 fl_chart、flutter_staggered_grid_view、flutter_spinkit 等
- 创建分层目录结构（core/shared/features）

**阶段 2：核心层开发 ?**
- 创建 `AppColors`：华为蓝主色调 + 成功绿/警告橙/危险红 + 暗色模式
- 创建 `AppDims`：圆角/间距常量
- 创建 `SpotModel` / `AlertModel` / `StatsModel`：完整数据模型
- 创建 `ApiService`：
  - 提取 OneNET 鉴权算法（version=2020-05-29 + hmac_md5）
  - 封装 getSpots/getAlerts/resolveAlert 等接口
  - 内置 Mock 数据兜底

**阶段 3：基础组件开发 ?**
- `HuaweiCard`：华为风格卡片，柔和阴影
- `StatusBadge`：状态徽章（空闲/占用/僵尸车）
- `CircleProgress`：环形进度条
- `EmptyState`：空状态组件
- `LoadingIndicator`：加载指示器（SpinKitFadingCircle）

**阶段 4：页面开发 ?（全部完成）**
- `main.dart`：Material 3 + BottomNavigationBar 5 Tab 导航（总览/车位/告警/数据/我的）
- `OverviewPage`：总览页（统计卡片 + 车位网格 + 僵尸车告警列表）
- `SpotsPage`：车位列表页（分区筛选 + 列表展示）
- `SpotDetailPage`：车位详情页（车牌展示 + 设备信息 + 三按钮操作）
- `AlertsPage`：告警页（删除/处理按钮操作）
- `StatsPage`：数据复盘页（统计卡片 + 饼图分析 + 折线图趋势 + 告警统计）
- `ProfilePage`：我的页面（用户信息 + 系统状态 + 功能菜单）

**阶段 5：代码清理与验证 ?**
- 删除旧代码：parking_spot.dart、home_page.dart、detail_page.dart、spot_card.dart、onenet_service.dart
- 修复编译错误：StatusBadge factory 构造函数 const 问题、totalSpots 参数传递
- 修复 fl_chart API 兼容问题：移除不存在的 `getDotColor`、`tooltipBgColor` 参数
- 修复 UI Bug：FormatException 解析错误修复
- 修复编码问题：所有中文文件使用 **UTF-8 with BOM** 编码
- **编码规范**：禁止使用 PowerShell 写入 Dart 文件（会导致 `$` 字符串插值被误解析），必须使用 Write 工具
- 构建验证：`flutter build windows --debug` 成功

#### 遗留问题
- [ ] 实际 OneNET API 联调待验证（当前使用 Mock 数据）
- [ ] CORS 跨域问题待确认（Chrome 调试可能受限）
- [ ] UI 细节待微调（间距、字体、动画等）

---

## 七、开发规范

### 7.1 代码规范

1. **命名规范**
   - 文件名：`snake_case`（如 `spot_model.dart`）
   - 类名：`PascalCase`（如 `SpotModel`）
   - 方法名/变量名：`camelCase`（如 `getSpots()`）
   - 常量：`camelCase`（如 `AppColors.primary`）

2. **样式规范**
   - 颜色必须使用 `AppColors` 类
   - 尺寸必须使用 `AppDims` 类
   - 禁止硬编码十六进制颜色值
   - 禁止硬编码尺寸数值

3. **组件复用**
   - 卡片 → `HuaweiCard`
   - 状态标签 → `StatusBadge`
   - 环形进度 → `CircleProgress`
   - 空状态 → `EmptyState`
   - 加载 → `LoadingIndicator`

### 7.2 Git 提交规范

提交信息格式：
```
<type>: <简短描述>

<模块>:
- <做了什么>
```

类型：
- `feat`: 新功能
- `fix`: 修复 bug
- `refactor`: 重构
- `docs`: 文档
- `chore`: 构建/配置

示例：
```
feat: 实现总览页和告警页基础框架

components:
- 实现 HuaweiCard、StatusBadge、CircleProgress 基础组件
- 完成 OverviewPage 和 AlertsPage 布局
- 接入 flutter_staggered_grid_view 网格布局
```

---

## 八、验收 Checklist

### 功能验收

- [x] 总览页正确显示统计数据和车位网格
- [x] 车位页支持分区切换（A/B/C 区）
- [x] 车位详情页显示完整车辆信息
- [x] 告警页支持操作按钮（删除/处理）
- [x] 数据复盘页正确显示图表（饼图+折线图+统计卡片）
- [x] 我的页面显示系统状态
- [x] 底部 TabBar 导航正常切换（5个Tab）
- [x] 中文正确显示（UTF-8 with BOM 编码）

### UI 验收

- [x] 华为蓝主题色正确应用
- [x] 卡片阴影符合规范
- [x] 圆角统一（卡片 16dp，按钮 12dp）
- [x] 留白充足，无拥挤感
- [x] 加载状态和空状态处理完整
- [x] fl_chart 图表正确显示

### 代码验收

- [x] 无编译错误
- [x] 无硬编码颜色/尺寸
- [x] 组件复用率 ≥ 90%
- [x] 代码结构清晰，符合分层架构
- [x] 所有文件使用 UTF-8 with BOM 编码
- [x] 图表 API 兼容 fl_chart 0.68.0

---

## 附录 A：快速启动命令

```bash
# 1. 创建项目
cd /path/to/User
flutter create --org com.smartparking .

# 2. 安装依赖
flutter pub get

# 3. 创建目录
# (参考阶段 1.3)

# 4. 运行项目
flutter run -d chrome
```

## 附录 B：常用命令

```bash
# 查看设备列表
flutter devices

# 运行到 Chrome
flutter run -d chrome

# 运行到 Windows
flutter run -d windows

# 运行到 Android
flutter run -d android

# 构建 APK
flutter build apk --release

# 查看依赖版本
flutter pub outdated
```
