# 路边僵尸车检测系统 · APP 开发文档（合并版 v3.0）

> **版本**：v3.0（合并版，整合设计/开发/架构三份旧文档）
> **更新日期**：2026-08-25
> **状态**：基础功能已完成，待办见「六、开发进度」
> **旧文档归档**：`User/archive/`（APP设计文档_旧版 / APP开发文档_旧版 / APP架构文档_旧版）

---

## 一、项目概览

### 1.1 项目简介

基于 Flutter 的「路边僵尸车检测系统」管理端 APP，用于停车场的实时监控、车位管理、僵尸车预警与处置、数据统计分析。

### 1.2 技术栈

| 类别     | 技术              | 版本    |
| -------- | ----------------- | ------- |
| 框架     | Flutter           | 3.47.0  |
| 语言     | Dart              | 3.13.0  |
| 状态管理 | Provider          | ^6.1.0  |
| 网络请求 | http              | ^1.2.0  |
| 图表     | fl_chart          | ^0.68.0 |
| 滑动列表 | flutter_slidable  | ^3.1.0  |
| 错位网格 | flutter_staggered_grid_view | ^0.7.0 |
| 轮播     | flutter_swiper_view | ^1.1.8 |
| 动画文字 | animated_text_kit | ^4.2.2  |
| 加载动画 | flutter_spinkit   | ^5.2.0  |
| 加密     | crypto            | ^3.0.3  |
| 本地存储 | shared_preferences | 2.2.2  |
| 后端平台 | 中国移动 OneNET（REST + HMAC-MD5 签名鉴权） | — |

### 1.3 项目结构（最新）

```
lib/
├── core/                              # 核心模块
│   ├── theme/
│   │   ├── app_colors.dart            # 颜色常量
│   │   └── app_dims.dart              # 尺寸常量
│   ├── models/
│   │   ├── spot_model.dart            # 车位/节点设备模型
│   │   ├── alert_model.dart           # 告警/工单模型
│   │   └── stats_model.dart           # 统计模型
│   ├── providers/
│   │   └── parking_provider.dart      # 全局唯一数据层（轮询/模式/告警/OTA）
│   └── services/
│       └── api_service.dart           # OneNET API 服务
├── shared/
│   └── widgets/                       # 公共组件（10 个）
│       ├── custom_app_bar.dart        # 蓝色渐变导航栏
│       ├── card_container.dart        # 卡片容器（基石组件）
│       ├── list_tile_base.dart        # 列表项基类
│       ├── action_button_group.dart   # 派单/通知/处置按钮组
│       ├── stat_card.dart             # 统计卡片
│       ├── huawei_card.dart           # 华为风格卡片（保留兼容）
│       ├── status_badge.dart          # 状态徽章（factory 构造）
│       ├── circle_progress.dart       # 圆环进度
│       ├── empty_state.dart           # 空状态
│       └── loading_indicator.dart     # 加载动画
├── features/
│   ├── overview/                      # 总览页（Tab0）
│   ├── spots/                         # 车位页（Tab1）+ 车位详情页
│   ├── alerts/                        # 告警中心（Tab2）
│   ├── stats/                         # 数据页（Tab3）
│   └── profile/                       # 我的页（Tab4）+ 固件升级页 + OTA 弹窗
└── main.dart                          # 入口 + MainShell（5 Tab + OTA 全局弹窗）
```

---

## 二、设计规范

### 2.1 整体风格

- 设计语言：Material 3 + 华为智慧生活式「高端科技感」
- 主色调：华为蓝 `#007DFF`（Primary），辅色浅蓝 `#E8F4FD`
- 背景色：浅灰白 `#F5F7FA`（非纯白，减少刺眼）
- 强调色：成功绿 `#00C781`、警告橙 `#FF9F43`、危险红 `#FF5B5B`
- 文字：主文字 `#1A1A1A`、次要文字 `#8A8A8A`
- 圆角：卡片 16dp、按钮 12dp、小标签 8dp
- 阴影：卡片 `blurRadius=12, offset=(0,2), opacity=0.08`
- 留白：页面边距 20dp，卡片间距 12dp
- 主页顶栏：大标题风格（28sp w700）+ 右侧图标
- 二级页顶栏：`CustomAppBar` 蓝色渐变 + 返回按钮
- 底部导航：5 个 Tab（首页/车位/告警/数据/我的），选中态蓝色高亮 + 图标放大

### 2.2 设计令牌

颜色统一走 `AppColors`，尺寸统一走 `AppDims`，**禁止硬编码颜色值与尺寸**。

```dart
// lib/core/theme/app_colors.dart
class AppColors {
  static const Color primary = Color(0xFF007DFF);
  static const Color primaryLight = Color(0xFFE8F4FD);
  static const Color background = Color(0xFFF5F7FA);
  static const Color surface = Colors.white;
  static const Color success = Color(0xFF00C781);
  static const Color warning = Color(0xFFFF9F43);
  static const Color danger = Color(0xFFFF5B5B);
  static const Color textPrimary = Color(0xFF1A1A1A);
  static const Color textSecondary = Color(0xFF8A8A8A);
  // 暗色主题：darkBackground/darkSurface/darkTextPrimary/darkTextSecondary
}

// lib/core/theme/app_dims.dart
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

### 2.3 组件使用规范

| 场景     | 组件                          |
| -------- | ----------------------------- |
| 卡片     | `CardContainer`（优先）/ `HuaweiCard`（预留） |
| 状态徽章 | `StatusBadge`（支持 factory + fromStatus） |
| 列表项   | `ListTileBase`                |
| 按钮组   | `ActionButtonGroup`           |
| 圆环进度 | `CircleProgress`              |
| 空状态   | `EmptyState`                  |
| 加载     | `LoadingIndicator`            |
| 导航栏   | `CustomAppBar`                |

### 2.4 状态徽章色

| 状态     | 颜色 | 说明     |
| -------- | ---- | -------- |
| free     | 绿色 | 空闲车位 |
| occupied | 橙色 | 已占用   |
| zombie   | 红色 | 僵尸车   |
| offline  | 灰色 | 设备离线 |
| pending  | 黄色 | 待处理   |
| resolved | 绿色 | 已处置   |

### 2.5 禁止事项

- 不要用 Material 2 默认深紫粉主题
- 不要用尖锐直角按钮
- 不要堆砌功能入口
- 不要用刺眼的纯白背景
- 不要硬编码颜色值 / 尺寸值（统一 `AppColors` / `AppDims`）
- 分割线用 `0.5dp + 35% 透明度`，不要用实线深色 Divider

### 2.6 依赖库

`fl_chart`（环形/折线图）、`flutter_slidable`（滑动操作）、`flutter_spinkit`（加载动画）、`crypto`（HMAC-MD5 签名）等，见 1.2 技术栈表。

### 2.7 AI 指令模板（可直接复制）

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
4. 数据来源：所有页面必须从 ParkingProvider 读取数据，禁止直接调用 ApiService。
5. 状态处理：数据加载时显示 LoadingIndicator，无数据时显示 EmptyState。
6. 导航规范：二级页（详情页/固件升级页）使用 CustomAppBar 蓝色渐变导航栏，
   主页使用大标题风格（Text 28sp w700 + 图标）。
7. 禁止行为：不要使用 Material 2 的默认样式，不要使用尖锐直角，不要堆砌功能入口，
   背景色必须为 AppColors.background。
当前任务：[在此处描述你要AI做的具体事情]
```

---

## 三、工程构建配置

### 3.1 Gradle / AGP / Kotlin 版本

| 配置项 | 值        | 说明                 |
| ------ | --------- | -------------------- |
| Gradle | 9.4.1-bin | 与 AGP 8.x 兼容      |
| AGP    | 8.11.1    | Flutter 3.47 最低要求 |
| Kotlin | 2.3.0     | 稳定版               |

### 3.2 settings.gradle.kts 关键配置

```kotlin
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)  // 关键：允许插件添加仓库
    repositories {
        maven { url = uri("https://storage.flutter-io.cn/download.flutter.io") }  // Flutter 引擎中国镜像
        maven { url = uri("https://storage.googleapis.com/download.flutter.io") }  // Flutter 引擎官方
        maven { url = uri("https://mirrors.cloud.tencent.com/nexus/repository/maven-public/") }
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        google()
        mavenCentral()
    }
}

plugins {
    id("dev.flutter.flutter-plugin-loader") version "1.0.0"
    id("com.android.application") version "8.11.1" apply false
    id("org.jetbrains.kotlin.android") version "2.3.0" apply false
}
```

### 3.3 gradle.properties 关键配置

```properties
org.gradle.jvmargs=-Xmx8G -XX:MaxMetaspaceSize=4G -XX:ReservedCodeCacheSize=512m -XX:+HeapDumpOnOutOfMemoryError
android.useAndroidX=true
android.newDsl=false              # 关键：AGP 8.x 必须为 false，否则 Flutter plugin 类型转换错误
android.builtInKotlin=false
org.gradle.internal.http.connectionTimeout=30000
org.gradle.internal.http.socketTimeout=30000
org.gradle.internal.http.connectionRetries=2
org.gradle.caching=true
android.overridePathCheck=true    # 关键：Windows 中文路径必须开启
```

### 3.4 构建要点

1. **PREFER_SETTINGS 而非 FAIL_ON_PROJECT_REPOS**：Flutter 插件会添加自己的仓库，FAIL_ON_PROJECT_REPOS 会导致构建失败。
2. **newDsl=false**：AGP 9+ 才完全支持 newDsl，AGP 8.x 下开启会导致 Flutter Gradle Plugin 类型转换错误（`ApplicationExtensionImpl$AgpDecorated_Decorated cannot be cast to AbstractAppExtension`）。
3. **Flutter 引擎仓库必须配置**：`flutter_embedding_debug` 等 artifact 不在普通 Maven 仓库，必须加 `storage.flutter-io.cn/download.flutter.io` 或 `storage.googleapis.com/download.flutter.io`。
4. **overridePathCheck=true**：Windows 下项目路径包含中文字符时必须开启。
5. **中国镜像**：阿里云 `maven.aliyun.com`、腾讯云 `mirrors.cloud.tencent.com`、Flutter 中国 `storage.flutter-io.cn`。

### 3.5 常见构建问题与解决

| 问题 | 解决方案 |
| ---- | -------- |
| `The non-ASCII space character U+FEFF can only be used in strings and comments.`（BOM 头混入 Dart 文件） | 用 PowerShell 批量移除 BOM（见下） |
| Gradle 插件下载超时/失败 | 配置中国镜像（见 3.2） |
| `AGP version is lower than Flutter's minimum supported version of 8.11.1` | AGP 升级到 8.11.1 |
| `Could not find io.flutter:flutter_embedding_debug:1.0.0-xxx` | 添加 Flutter 引擎仓库 |
| `Your project path contains non-ASCII characters` | `gradle.properties` 加 `android.overridePathCheck=true` |

BOM 移除脚本：

```powershell
Get-ChildItem -Path "lib" -Recurse -Filter "*.dart" | ForEach-Object {
    $content = [System.IO.File]::ReadAllText($_.FullName)
    if ($content.Length -gt 0 -and $content[0] -eq [char]0xFEFF) {
        $clean = $content.TrimStart([char]0xFEFF)
        [System.IO.File]::WriteAllText($_.FullName, $clean)
        Write-Host "Fixed: $($_.FullName)"
    }
}
```

### 3.6 常用命令

```bash
flutter pub get              # 获取依赖
flutter clean                # 清理构建
flutter run -d <设备ID>      # 运行（debug）
flutter devices              # 查看设备列表
flutter build apk --release  # 构建 release APK
flutter pub outdated         # 检查依赖更新
```

### 3.7 注意事项

1. **不要用 PowerShell 写 Dart 文件** — 使用 Write 工具，避免 BOM 问题。
2. **Windows 中文路径** — 确保 `android.overridePathCheck=true`。
3. **中国网络环境** — 确保镜像配置正确。
4. **Gradle 版本** — 使用 9.4.1-bin（已缓存）。
5. **热重载** — 修改 `main.dart` 主题 / 静态常量 / 新增删除文件时热重载不生效，需按 `R` 完全重启。

---

## 四、当前架构实现

### 4.1 导航结构（5 Tab）

```
MaterialApp（ChangeNotifierProvider<ParkingProvider>，routes: {'/alerts': AlertsPage}）
└── MainShell（Scaffold + 自定义底部导航）
    ├── [0] 首页 OverviewPage
    ├── [1] 车位 SpotsPage ──push──> SpotDetailPage（车位详情）
    ├── [2] 告警 AlertsPage（告警中心）
    ├── [3] 数据 StatsPage（数据复盘）
    └── [4] 我的 ProfilePage ──> FirmwareUpgradePage（固件升级）
    全局：OTA 升级弹窗（OtaUpgradeDialog，MainShell 统一触发）
```

### 4.2 全局状态 ParkingProvider

`lib/core/providers/parking_provider.dart` — **唯一的全局数据层**，所有页面从它读取，禁止直接调 ApiService。

- 唯一持有车位数据 `spots` + **唯一 3s 轮询定时器**
- **真实/本地模式**（`realOnly`，SharedPreferences 持久化）：本地模式 = 真实设备 + mock 补齐 9 台；真实模式 = 仅真实设备。车位页点「车位」标题切换
- **告警派生**：从车位数据派生（占用 ≥24h 告警、≥72h 僵尸车），维护处理/忽略状态
- **处理记录管理**：僵尸车离开车位自动标记「已处理」（快照车牌+处理时间）；新车进入空位清除旧处理记录；已处理/已忽略不参与首页推送但保留历史
- **批量选中集合**（跨页同步）
- **OTA 检测**：短窗口策略（启动/前台 1 次 + 5s×9 共 10 次后自动停止），检测到待升级任务（未忽略）时置 `otaPromptVisible`
- 一周趋势为演示用静态数据（图表占位，非平台真实统计）

### 4.3 页面要点

| 页面 | 要点 |
| ---- | ---- |
| 总览 OverviewPage | 问候卡、车位概览四格、饼图（空闲/占用/僵尸）、最新僵尸车告警卡、下拉刷新 |
| 车位 SpotsPage | A/B 区筛选（不显示 C 区）、隐藏模式开关（点「车位」标题）、离线设备灰显+云图标+隐藏3点菜单、3点菜单仅僵尸车（查看详情）、点击进详情 |
| 车位详情 SpotDetailPage | 垂直分区：车位信息 → 车辆信息 → 设备信息 → 告警详情 → 处理操作区；处理操作区仅僵尸车显示，含[通知车主][派单→处理人选择 Dialog] |
| 告警 AlertsPage | 状态筛选（全部/待处理/处理中/已处理/已忽略）、告警卡片、删除+处理按钮、空状态/加载/下拉刷新 |
| 数据 StatsPage | 顶部概览双卡、车位占用饼图、一周趋势折线图（mock）、告警总数卡 |
| 我的 ProfilePage | 用户卡、快捷操作、功能分组（系统/运维/账户）；固件升级入口 → FirmwareUpgradePage |

### 4.4 数据模型

**SpotModel**（车位/节点设备）
- 字段：`id / zone(A/B/C) / status(free|occupied|zombie|offline) / occupiedHours / batteryLevel / signalStrength / plateNumber / lastUpdated / isOnline` + 本地模拟字段（notifyStatus/handlerName/handledAt）
- 分区映射：设备编号 ≤3=A区，≤6=B区，其余=C区
- 状态判定：离线 → `offline`；`ParkStatus=1` → occupied；`ParkStatus=2` → zombie；否则 free
- ⚠️ `batteryLevel / signalStrength` 仍为硬编码（在线 85.0/-65，离线 0），见待办

**AlertModel**（告警/工单）
- 字段：`id / plateNumber / spotId / occupiedHours / status(pending|dispatched|resolved) / imageUrl? / createdAt?`
- 由 `ParkingProvider` 从车位数据本地合成，非独立后端接口

**StatsModel**（统计）
- 字段：`totalSpots / occupiedSpots / zombieSpots / occupancyRate / weeklyTrend(List<DailyTrend>)`
- `weeklyTrend` 为演示静态数据（7 天 mock），非平台真实统计

### 4.5 API 服务（OneNET 对接层）

`lib/core/services/api_service.dart`

| 方法 | 实现 |
| ---- | ---- |
| `getSpots({realOnly})` | `/device/list`（节点产品 `04kjwU9TC7`，网关产品不纳入车位）→ `/thingmodel/query-device-property` 逐个查属性 → 排序/去重 → 按模式补 mock 或仅真实 |
| `_generateAuthorization()` | ✅ 已正确实现：`StringToSign = et\nmethod\nres\nversion`，`res=userid/528332`（末尾无斜杠、无尾换行），HMAC-MD5 + base64，完整拼进 Authorization 头 |
| `getDeviceDetail()` | 设备详情查询，失败走 mock |
| `setProperty()` | `/thingmodel/set-device-property` 下发（如 OtaAllow/LED） |
| `getAlerts()` | 由 getSpots 本地合成（≥24h 告警） |
| `dispatchAlert/notifyOwner/resolveAlert` | 本地动作（内存态），无后端持久化 |
| `getStats()` | 半合成：统计来自 getSpots，weeklyTrend 为 mock |
| `getOtaTaskStatus()` | `/fuse-ota/{pro_id}/{dev_name}/{tid}/check` 轮询升级状态 |

网关：产品 `9YIs0S7V11`，设备 `PGW001`（承载 OtaAllow 全网升级确认门控）。
设备名使用 OneNET `name` 字段真实名称；mock 用 `Park001~009` 命名。

---

## 五、OneNET 平台对接

### 5.1 本项目实际使用的接口

| 分类 | URL | 用途 |
| ---- | --- | ---- |
| 设备管理 | `/device/list` | 读取全部车位设备（含离线） |
| 物模型使用 | `/thingmodel/query-device-property` | 读取 ParkStatus/Ultrasonic/OccupiedTime 等属性（支持离线设备读存储数据） |
| 物模型使用 | `/thingmodel/set-device-property` | 下发属性（LED/LedEnable/OtaAllow 等） |
| OTA 南向 | `/fuse-ota/{pro_id}/{dev_name}/{tid}/check` | 查询升级任务状态 |
| OTA 南向 | `/fuse-ota/{pro_id}/{dev_name}/version` | 上报/查看节点固件版本 |

> 全量 OneNET 平台接口清单见归档文档 `User/archive/APP架构文档_旧版.md`（4.1 节）。

### 5.2 OTA 升级状态接口详解

**接口**：`GET https://iot-api.heclouds.com/fuse-ota/{pro_id}/{dev_name}/{tid}/check`

**请求头**：`Authorization: version=2022-05-01&res=userid%2F{userId}&et=...&method=sha1&sign=...`（用户级签名）

**响应**：
```json
{ "code": 0, "msg": "succ", "data": { "status": 1 } }
```
`status`：1=待升级、2=下载中、3=升级中、4=升级成功、5=升级失败、6=升级取消

**App 实现对照**（`getOtaTaskStatus`）：
- URL 与文档一致；Authorization 使用用户级签名
- 正确解包 `data.status`，供 ParkingProvider 轮询驱动弹窗 / 固件升级页状态显示
- ⚠️ 实测：`status` 在执行期间可能一直为 1（待升级），直到任务完成才变 4，不返回实时进度 step

---

## 六、开发进度

### 6.1 已完成功能

**数据 & 平台对接**
- [x] OneNET 签名鉴权正确（StringToSign = et\nmethod\nres\nversion，res 无尾斜杠、无尾换行）
- [x] 真实读取设备列表 + 设备属性，离线设备读平台存储数据
- [x] 真实/本地模式切换（SharedPreferences 持久化），真实+模拟补齐 9 台
- [x] 设备名用 OneNET 真实 `name`；mock 用 Park001~009
- [x] 网关（PGW001）承载 OtaAllow 全网升级门控，不纳入车位列表

**全局状态**
- [x] ParkingProvider 单一数据层，唯一 3s 轮询，跨页实时同步
- [x] 告警派生（≥24h 告警 / ≥72h 僵尸车）与处理/忽略状态维护
- [x] 僵尸车离开车位自动标记已处理（快照车牌+时间）；新车进入清除旧处理记录
- [x] 首页推送排除已处理/已忽略告警，历史保留在告警中心

**页面功能**
- [x] 5 Tab 导航（首页/车位/告警/数据/我的）
- [x] 车位页：A/B 区筛选、隐藏模式开关、离线灰显+云图标、3点菜单仅僵尸车
- [x] 车位详情页：垂直分区布局，处理操作区仅僵尸车（通知/派单+处理人选择）
- [x] 告警中心：状态筛选（全部/待处理/处理中/已处理/已忽略）
- [x] 数据页：饼图 + 趋势图（趋势为 mock 占位）
- [x] 路由 `/alerts` 已注册

**OTA 升级**
- [x] 短窗口检测：启动/前台 1 次 + 5s×9 共 10 次后自动停止
- [x] 全局弹窗（MainShell 触发）：动态标题（检测到新固件/固件升级中/固件升级完成）、完成按钮、忽略列表防重复
- [x] 升级状态轮询至 status≥4 复位；任意操作即取消检测轮询
- [x] 固件升级页（FirmwareUpgradePage）

### 6.2 未完成 / 待办清单

**P0 安全**
- [ ] **AccessKey 明文硬编码**在 `api_service.dart`（`_accessKey`）→ 反编译 APK 即可提取，等于交出 OneNET 账户权限。应自建后端中转（Mobile → HTTPS → Backend → OneNET），前端只持有用户级令牌

**P1 数据真实性**
- [ ] `batteryLevel / signalStrength` 硬编码（在线 85.0/-65）→ 从 OneNET 属性读取或改「未知」样式
- [ ] 一周趋势为静态 mock → 接平台历史统计或改占位「暂无历史数据」
- [ ] 告警处理记录仅内存，重启丢失 → 需后端持久化（或 OneNET Data Stream）

**P2 功能完善**
- [ ] Profile 页 14 个入口空实现（快捷操作/系统/运维/账户分组）
- [ ] 车位页 C 区：模型分区映射仍含 C，前端已隐藏，需确认分区策略
- [ ] 设备属性查询为逐个串行 HTTP，设备多时首屏慢 → 改为并发或批量接口

**工程质量**
- [ ] 单元测试 / Widget 测试缺失（SpotModel.fromJson、24h/72h 判定、分区映射适合单测）
- [ ] `flutter analyze` / `dart format` 未纳入 CI 卡点

**长期方向**
- [ ] 真推送（OneNET 告警 → 后端 → 厂商推送通道）
- [ ] 国际化 i18n
- [ ] 离线模式（Hive/isar 缓存最近车位快照）
- [ ] 工单操作审计流水（谁何时改了什么状态）

---

## 七、代码规范 & Git 提交规范

### 7.1 命名规范

- 文件名：`snake_case`（如 `spot_model.dart`）
- 类名：`PascalCase`（如 `SpotModel`）
- 方法/变量：`camelCase`（如 `getSpots()`）
- 常量：`camelCase`（如 `AppColors.primary`）

### 7.2 Git 提交规范

```
<类型>: <简要概述核心改动>

<模块>:
- <做了什么>（动词开头，一句话说清做了什么 + 解决什么问题）
```

**类型**：`feat`（新功能）/ `fix`（修复 bug）/ `refactor`（重构）/ `docs`（文档变更）/ `chore`（杂项/构建配置）

**示例**：
```
feat: 实现车位详情页与预警处置功能

components:
- 实现 SpotDetailPage，展示车位详细信息
- 实现 ActionButtonGroup，支持派单/通知/处置操作
- 接入 flutter_slidable 实现滑动删除
```

---

## 八、关键文件索引

| 类型 | 文件 |
| ---- | ---- |
| 入口 & 导航 | `lib/main.dart` |
| 全局数据层 | `lib/core/providers/parking_provider.dart` |
| API 服务 | `lib/core/services/api_service.dart` |
| 模型 | `lib/core/models/spot_model.dart` / `alert_model.dart` / `stats_model.dart` |
| 主题 | `lib/core/theme/app_colors.dart` / `app_dims.dart` |
| 总览页 | `lib/features/overview/presentation/pages/overview_page.dart` |
| 车位页 | `lib/features/spots/presentation/pages/spots_page.dart` |
| 车位详情页 | `lib/features/spots/presentation/pages/spot_detail_page.dart` |
| 告警中心 | `lib/features/alerts/presentation/pages/alerts_page.dart` |
| 数据页 | `lib/features/stats/presentation/pages/stats_page.dart` |
| 我的页 | `lib/features/profile/presentation/pages/profile_page.dart` |
| 固件升级页 | `lib/features/profile/presentation/pages/firmware_upgrade_page.dart` |
| OTA 弹窗 | `lib/features/profile/presentation/widgets/ota_upgrade_dialog.dart` |
| 公共组件 | `lib/shared/widgets/*` |
| 依赖配置 | `pubspec.yaml` |
