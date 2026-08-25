# 路边僵尸车检测系统 - APP 开发文档

> **版本**: v2.0（中文增强版）
> **日期**: 2026-08-19
> **代码规范**: 参考 `APP开发规范.md`
> **状态**: 开发中

---

## 一、项目概述

### 1.1 项目简介

本项目是一个基于 Flutter 的"路边僵尸车检测系统"管理端 APP，主要用于停车场的实时监控、车位管理、僵尸车预警、数据统计分析等功能。

### 1.2 技术栈

| 类别     | 技术                        | 版本    |
| -------- | --------------------------- | ------- |
| 框架     | Flutter                     | 3.47.0  |
| 语言     | Dart                        | 3.13.0  |
| 状态管理 | setState / Provider         | -       |
| 网络请求 | http                        | ^1.2.0  |
| 图表     | fl_chart                    | ^0.68.0 |
| 滑动列表 | flutter_slidable            | ^3.1.0  |
| 错位网格 | flutter_staggered_grid_view | ^0.7.0  |
| 轮播     | flutter_swiper_view         | ^1.1.8  |
| 动画文字 | animated_text_kit           | ^4.2.2  |
| 加载动画 | flutter_spinkit             | ^5.2.0  |
| 加密     | crypto                      | ^3.0.3  |

### 1.3 项目结构

```
lib/
├── core/                          # 核心模块
│   ├── theme/
│   │   ├── app_colors.dart        # 颜色常量
│   │   └── app_dims.dart          # 尺寸常量
│   ├── models/
│   │   ├── spot_model.dart        # 车位模型
│   │   ├── alert_model.dart       # 预警模型
│   │   └── stats_model.dart       # 统计模型
│   └── services/
│       └── api_service.dart       # API 服务
├── shared/                        # 公共组件
│   └── widgets/
│       ├── custom_app_bar.dart    # 自定义导航栏
│       ├── card_container.dart    # 卡片容器
│       ├── list_tile_base.dart    # 列表项基类
│       ├── action_button_group.dart # 操作按钮组
│       ├── stat_card.dart         # 统计卡片
│       ├── huawei_card.dart       # 华为风格卡片
│       ├── status_badge.dart      # 状态徽章
│       ├── circle_progress.dart   # 圆环进度
│       ├── empty_state.dart       # 空状态
│       └── loading_indicator.dart # 加载指示器
├── features/
│   ├── overview/                  # 总览页
│   │   └── presentation/pages/overview_page.dart
│   ├── spots/                     # 车位页
│   │   └── presentation/pages/
│   │       ├── spots_page.dart
│   │       └── spot_detail_page.dart
│   ├── alerts/                    # 预警页
│   │   └── presentation/pages/alerts_page.dart
│   ├── stats/                     # 数据分析页
│   │   └── presentation/pages/stats_page.dart
│   └── profile/                   # 我的页
│       └── presentation/pages/profile_page.dart
└── main.dart                      # 入口文件
```

---

## 二、Android 构建配置（最终版）

### 2.1 Gradle 版本

| 配置项 | 值        | 说明                  |
| ------ | --------- | --------------------- |
| Gradle | 9.4.1-bin | 与 AGP 8.x 兼容       |
| AGP    | 8.11.1    | Flutter 3.47 最低要求 |
| Kotlin | 2.3.0     | 稳定版                |

### 2.2 settings.gradle.kts 关键配置

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

### 2.3 gradle.properties 关键配置

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

### 2.4 构建要点说明

1. **PREFER_SETTINGS 而非 FAIL_ON_PROJECT_REPOS**

   - Flutter 插件会添加自己的仓库，FAIL_ON_PROJECT_REPOS 会导致构建失败
   - 改用 PREFER_SETTINGS 后，以 settings 中配置的仓库为主
2. **newDsl=false 的原因**

   - AGP 9+ 才完全支持 newDsl，AGP 8.x 下开启会导致 Flutter Gradle Plugin 类型转换错误
   - 错误信息：`ApplicationExtensionImpl$AgpDecorated_Decorated cannot be cast to AbstractAppExtension`
3. **Flutter 引擎仓库必须配置**

   - `flutter_embedding_debug` 等 artifact 不在普通 Maven 仓库
   - 必须添加 `storage.flutter-io.cn/download.flutter.io` 或 `storage.googleapis.com/download.flutter.io`
4. **overridePathCheck=true**

   - Windows 下项目路径包含中文字符时必须开启
   - 本项目路径：`我的/智能停车场`
5. **中国镜像配置**

   - 阿里云：`maven.aliyun.com`
   - 腾讯云：`mirrors.cloud.tencent.com`
   - Flutter 中国：`storage.flutter-io.cn`

---

## 三、构建问题与解决方案

### 3.1 BOM 编码问题

**问题描述**：

```
Error: The non-ASCII space character U+FEFF can only be used in strings and comments.
import 'package:flutter/material.dart';
^
```

**原因**：使用 Write 工具保存 Dart 文件时带入了 UTF-8 BOM 头。

**解决方案**（PowerShell）：

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

### 3.2 Gradle 插件下载超时

**问题**：国内下载 Gradle 插件极慢或失败。

**解决方案**：配置中国镜像（见 2.2 节）。

### 3.3 AGP 版本不兼容

**问题描述**：

```
AGP version is lower than Flutter's minimum supported version of 8.11.1
```

**解决方案**：AGP 升级到 8.11.1（settings.gradle.kts 中配置）。

### 3.4 Flutter 引擎 artifact 找不到

**问题描述**：

```
Could not find io.flutter:flutter_embedding_debug:1.0.0-xxx
```

**解决方案**：添加 Flutter 引擎仓库 `storage.flutter-io.cn/download.flutter.io`。

### 3.5 中文路径问题

**问题描述**：

```
Your project path contains non-ASCII characters
```

**解决方案**：`gradle.properties` 添加 `android.overridePathCheck=true`。

---

## 四、开发流程

### 阶段 1：项目初始化（0.5 天）· 已完成

| 步骤 | 任务                 | 状态 | 输出文件     |
| ---- | -------------------- | ---- | ------------ |
| 1.1  | 创建 Flutter 项目    | ?    | 项目根目录   |
| 1.2  | 配置 pubspec.yaml    | ?    | pubspec.yaml |
| 1.3  | 建立目录结构         | ?    | lib/         |
| 1.4  | 配置 Material 3 主题 | ?    | main.dart    |

### 阶段 2：核心模块开发（1 天）· 已完成

| 步骤 | 任务            | 状态 | 输出文件             |
| ---- | --------------- | ---- | -------------------- |
| 2.1  | 实现 AppColors  | ?    | `app_colors.dart`  |
| 2.2  | 实现 AppDims    | ?    | `app_dims.dart`    |
| 2.3  | 实现 SpotModel  | ?    | `spot_model.dart`  |
| 2.4  | 实现 AlertModel | ?    | `alert_model.dart` |
| 2.5  | 实现 StatsModel | ?    | `stats_model.dart` |
| 2.6  | 实现 ApiService | ?    | `api_service.dart` |

### 阶段 3：公共组件开发（1 天）· 已完成

| 步骤 | 任务                   | 状态 | 输出文件                     |
| ---- | ---------------------- | ---- | ---------------------------- |
| 3.1  | 实现 CustomAppBar      | ?    | `custom_app_bar.dart`      |
| 3.2  | 实现 CardContainer     | ?    | `card_container.dart`      |
| 3.3  | 实现 ListTileBase      | ?    | `list_tile_base.dart`      |
| 3.4  | 实现 ActionButtonGroup | ?    | `action_button_group.dart` |
| 3.5  | 实现 StatCard          | ?    | `stat_card.dart`           |
| 3.6  | 实现 StatusBadge       | ?    | `status_badge.dart`        |
| 3.7  | 实现 CircleProgress    | ?    | `circle_progress.dart`     |
| 3.8  | 实现 EmptyState        | ?    | `empty_state.dart`         |
| 3.9  | 实现 LoadingIndicator  | ?    | `loading_indicator.dart`   |

### 阶段 4：页面开发（2.5 天）· 已完成

| 步骤 | 任务                          | 优先级 | 状态 | 输出文件                  |
| ---- | ----------------------------- | ------ | ---- | ------------------------- |
| 4.1  | 实现底部导航栏                | P0     | ?    | main.dart                 |
| 4.2  | 实现总览页 OverviewPage       | P0     | ?    | `overview_page.dart`    |
| 4.3  | 实现车位详情页 SpotDetailPage | P1     | ?    | `spot_detail_page.dart` |
| 4.4  | 实现预警页 AlertsPage         | P1     | ?    | `alerts_page.dart`      |
| 4.5  | 实现数据分析页 StatsPage      | P2     | ?    | `stats_page.dart`       |
| 4.6  | 实现我的页 ProfilePage        | P2     | ?    | `profile_page.dart`     |
| 4.7  | 实现车位页 SpotsPage          | P1     | ?    | `spots_page.dart`       |

### 阶段 5：数据联调与测试（1.5 天）· 进行中

| 步骤 | 任务         | 状态 | 说明                      |
| ---- | ------------ | ---- | ------------------------- |
| 5.1  | 模拟数据验证 | ?    | 使用 Mock 数据验证 UI     |
| 5.2  | API 接口对接 | ?    | 待后端就绪                |
| 5.3  | 状态管理完善 | ?    | 已实现加载/错误/空状态    |
| 5.4  | 页面路由导航 | ?    | 车位详情页跳转            |
| 5.5  | 性能优化     | ?    | 列表使用 ListView.builder |
| 5.6  | UI 细节打磨  | ?    | 华为风格统一              |

### 阶段 6：打包发布（1 天）· 待开始

| 步骤 | 任务         | 状态 | 说明          |
| ---- | ------------ | ---- | ------------- |
| 6.1  | Android 签名 | ?    | 创建 keystore |
| 6.2  | APK/AAB 打包 | ?    | release 构建  |
| 6.3  | 版本号管理   | ?    | 配置 version  |
| 6.4  | 应用商店发布 | ?    | 各平台上架    |

---

## 五、组件清单与依赖关系

### 5.1 组件清单

| 文件路径                                                        | 阶段   | 状态 | 说明         |
| --------------------------------------------------------------- | ------ | ---- | ------------ |
| `lib/core/theme/app_colors.dart`                              | 阶段 2 | ?    | 颜色常量     |
| `lib/core/theme/app_dims.dart`                                | 阶段 2 | ?    | 尺寸常量     |
| `lib/core/models/spot_model.dart`                             | 阶段 2 | ?    | 车位数据模型 |
| `lib/core/models/alert_model.dart`                            | 阶段 2 | ?    | 预警数据模型 |
| `lib/core/models/stats_model.dart`                            | 阶段 2 | ?    | 统计数据模型 |
| `lib/core/services/api_service.dart`                          | 阶段 2 | ?    | API 服务封装 |
| `lib/shared/widgets/custom_app_bar.dart`                      | 阶段 3 | ?    | 自定义导航栏 |
| `lib/shared/widgets/card_container.dart`                      | 阶段 3 | ?    | 卡片容器     |
| `lib/shared/widgets/list_tile_base.dart`                      | 阶段 3 | ?    | 列表项基类   |
| `lib/shared/widgets/action_button_group.dart`                 | 阶段 3 | ?    | 操作按钮组   |
| `lib/shared/widgets/stat_card.dart`                           | 阶段 3 | ?    | 统计卡片     |
| `lib/shared/widgets/huawei_card.dart`                         | 阶段 3 | ?    | 华为风格卡片 |
| `lib/shared/widgets/status_badge.dart`                        | 阶段 3 | ?    | 状态徽章     |
| `lib/shared/widgets/circle_progress.dart`                     | 阶段 3 | ?    | 圆环进度     |
| `lib/shared/widgets/empty_state.dart`                         | 阶段 3 | ?    | 空状态       |
| `lib/shared/widgets/loading_indicator.dart`                   | 阶段 3 | ?    | 加载指示器   |
| `lib/features/overview/presentation/pages/overview_page.dart` | 阶段 4 | ?    | 总览页       |
| `lib/features/spots/presentation/pages/spots_page.dart`       | 阶段 4 | ?    | 车位列表页   |
| `lib/features/spots/presentation/pages/spot_detail_page.dart` | 阶段 4 | ?    | 车位详情页   |
| `lib/features/alerts/presentation/pages/alerts_page.dart`     | 阶段 4 | ?    | 预警页       |
| `lib/features/stats/presentation/pages/stats_page.dart`       | 阶段 4 | ?    | 数据分析页   |
| `lib/features/profile/presentation/pages/profile_page.dart`   | 阶段 4 | ?    | 我的页       |
| `lib/main.dart`                                               | 阶段 1 | ?    | 应用入口     |

### 5.2 依赖关系

```
main.dart
  ├── overview_page.dart
  ├── spots_page.dart
  │   └── spot_detail_page.dart
  ├── alerts_page.dart
  ├── stats_page.dart
  └── profile_page.dart
       │
       ├── [共享组件层]
       │   ├── custom_app_bar.dart
       │   ├── card_container.dart
       │   ├── list_tile_base.dart
       │   ├── action_button_group.dart
       │   ├── stat_card.dart
       │   ├── huawei_card.dart
       │   ├── status_badge.dart
       │   ├── circle_progress.dart
       │   ├── empty_state.dart
       │   └── loading_indicator.dart
       │
       ├── [核心层]
       │   ├── app_colors.dart
       │   ├── app_dims.dart
       │   ├── spot_model.dart
       │   ├── alert_model.dart
       │   ├── stats_model.dart
       │   └── api_service.dart
```

---

## 六、UI 设计规范

### 6.1 设计风格

- **参考**：华为智慧生活 APP
- **主色调**：`#007DFF`（华为蓝）
- **设计语言**：Material Design 3
- **卡片圆角**：16dp
- **按钮圆角**：12dp

### 6.2 页面结构

| 页面       | 特色                         | 主要组件                              |
| ---------- | ---------------------------- | ------------------------------------- |
| 总览页     | KPI 卡片 + 圆环图 + 最新预警 | StatCard, CircleProgress, StatusBadge |
| 车位页     | 区域筛选 + 车位网格          | StatusBadge, CardContainer            |
| 预警页     | 滑动操作（派单/通知/处置）   | ActionButtonGroup, flutter_slidable   |
| 数据分析页 | 折线图 + 柱状图 + 饼图       | fl_chart                              |
| 我的页     | 用户卡片 + 功能分组 + 列表项 | ListTileBase, CardContainer           |

### 6.3 状态徽章类型

| 状态     | 颜色 | 说明     |
| -------- | ---- | -------- |
| free     | 绿色 | 空闲车位 |
| occupied | 蓝色 | 已占用   |
| zombie   | 橙色 | 僵尸车   |
| pending  | 黄色 | 待处理   |
| resolved | 灰色 | 已处置   |

---

## 七、编码规范

### 7.1 命名规范

1. **文件命名**

   - 文件名：`snake_case`，如 `spot_model.dart`
2. **类命名**

   - 类名：`PascalCase`，如 `SpotModel`
3. **方法/函数命名**

   - 方法名：`camelCase`，如 `getSpots()`
4. **常量命名**

   - 常量：`camelCase`，如 `AppColors.primary`

### 7.2 样式规范

- 颜色统一使用 `AppColors` 类
- 尺寸统一使用 `AppDims` 类
- 禁止硬编码颜色值
- 禁止硬编码尺寸值

### 7.3 组件使用规范

- 卡片 → `CardContainer` 或 `HuaweiCard`
- 状态徽章 → `StatusBadge`
- 圆环进度 → `CircleProgress`
- 空状态 → `EmptyState`
- 加载 → `LoadingIndicator`
- 列表项 → `ListTileBase`
- 导航栏 → `CustomAppBar`

---

## 八、Git 提交规范

### 提交信息格式

```
<type>: <简要概述核心改动>

<模块>:
- <做了什么>
```

### 类型

- `feat`: 新功能
- `fix`: 修复 bug
- `refactor`: 重构
- `docs`: 文档变更
- `chore`: 杂项/构建配置

### 示例

```
feat: 实现车位详情页与预警处置功能

components:
- 实现 SpotDetailPage，展示车位详细信息
- 实现 ActionButtonGroup，支持派单/通知/处置操作
- 接入 flutter_slidable 实现滑动删除
```

---

## 九、运行与调试

### 9.1 常用命令

```bash
# 获取依赖
flutter pub get

# 清理构建
flutter clean

# 运行（debug 模式）
flutter run -d <设备ID>

# 查看设备列表
flutter devices

# 构建 release APK
flutter build apk --release

# 检查依赖更新
flutter pub outdated
```

### 9.2 热重载

- 保存 Dart 文件后自动触发热重载
- 如需完全重启，在终端按 `R`（大写）
- 热重载无法生效的情况：
  - 修改了 `main.dart` 的主题配置
  - 修改了静态常量
  - 新增/删除了文件

### 9.3 注意事项

1. **不要用 PowerShell 写 Dart 文件** — 使用 Write 工具，避免 BOM 问题
2. **Windows 中文路径** — 确保 `android.overridePathCheck=true`
3. **中国网络环境** — 确保镜像配置正确（见第二节）
4. **Gradle 版本** — 使用 9.4.1-bin（已缓存）

---

## 十、完成清单

### 功能完成

- ? 总览页：统计卡片 + 车位汇总
- ? 车位页：区域筛选 A/B/C
- ? 车位详情页：详细信息 + 操作按钮
- ? 预警页：滑动操作处置
- ? 数据分析页：图表展示
- ? 我的页：用户信息 + 设置
- ? 底部导航栏：5 个 Tab
- ? 加载状态 / 空状态处理

### UI 完成

- ? 华为蓝色主题色
- ? 卡片阴影规范
- ? 圆角统一（卡片 16dp，按钮 12dp）
- ? 分割线可见性
- ? 状态徽章颜色区分
- ? fl_chart 图表展示

### 构建完成

- ? AGP 8.11.1 + Gradle 9.4.1
- ? 中国镜像配置
- ? BOM 问题修复
- ? 中文路径支持
- ? Flutter 引擎仓库配置

### 待完成

- ? OneNET API 真实对接
- ? CORS 跨域问题（Web 端）
- ? UI 在不同屏幕尺寸适配
- ? Android 签名与发布

---

## 附录 A：快速开始

```bash
# 1. 进入项目目录
cd path/to/User

# 2. 获取依赖
flutter pub get

# 3. 运行项目
flutter run -d <设备ID>

# 4. 或运行 Chrome
flutter run -d chrome
```

## 附录 B：设备调试

```bash
# 查看设备列表
flutter devices

# 运行到特定设备
flutter run -d 25102RKBEC

# 构建 APK
flutter build apk --release
```
