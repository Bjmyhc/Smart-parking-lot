# 智能停车场 APP

基于 Flutter 开发的智能停车场监控系统用户端，对接 OneNET 物联网云平台。

## 功能特性

- ? **实时车位状态**: 显示所有车位的实时占用状态
- ? **数据统计**: 总车位、空闲、占用、僵尸车数量统计
- ? **详细数据**: 超声波距离、地磁检测、占用时间等传感器数据
- ?? **远程控制**: 远程控制 LED 使能开关
- ? **跨平台**: 支持 Android 和 iOS

## 技术栈

- **Flutter 3.10+**
- **Dart 3.0+**
- **Provider** - 状态管理
- **HTTP** - 网络请求
- **OneNET API** - 物联网平台接口

## 快速开始

### 1. 安装 Flutter

```bash
# 访问 https://flutter.dev 下载 SDK
# 或使用 git clone
git clone https://github.com/flutter/flutter.git
cd flutter
./bin/flutter --version
```

### 2. 获取依赖

```bash
cd User
flutter pub get
```

### 3. 配置 OneNET API

编辑 `lib/services/onenet_service.dart`，修改以下配置：

```dart
static const String _productId = '你的产品ID';
static const String _gatewayName = '你的网关设备名';
static const String _apiKey = '你的API Key';
```

**获取 API Key 步骤**：
1. 登录 [OneNET 平台](https://open.iot.10086.cn/)
2. 进入你的产品
3. 点击「设备管理」→ 选择设备
4. 点击「API Key」→「创建新的 API Key」
5. 复制生成的 Key

### 4. 运行项目

```bash
# 检查环境
flutter doctor

# 连接 Android 设备（USB 调试或 WiFi ADB）
adb connect <设备IP>:5555

# 运行
flutter run

# 或者选择设备
flutter devices
flutter run -d <device_id>
```

### 5. 打包发布

#### Android APK

```bash
# Debug 版本
flutter build apk

# Release 版本（需要签名配置）
flutter build apk --release

# 输出路径
# build/app/outputs/flutter-apk/app-release.apk
```

#### Android AAB（Google Play 上架）

```bash
flutter build appbundle --release
```

#### iOS

```bash
# 需要 macOS 和 Xcode
flutter build ios

# 使用 Xcode 打开 ios/Runner.xcworkspace
# 配置签名后上传到 App Store Connect
```

## 项目结构

```
User/
├── lib/
│   ├── main.dart              # 入口文件
│   ├── models/
│   │   └── parking_spot.dart  # 数据模型 & 状态管理
│   ├── pages/
│   │   ├── home_page.dart     # 首页 - 车位列表
│   │   └── detail_page.dart  # 详情页 - 车位详情
│   ├── services/
│   │   └── onenet_service.dart # OneNET API 服务
│   └── widgets/
│       └── spot_card.dart    # 车位卡片组件
├── android/                    # Android 原生配置
├── ios/                        # iOS 原生配置
├── assets/                     # 静态资源
├── pubspec.yaml               # 项目配置
└── README.md
```

## OneNET 物模型

### 产品信息
- **产品 ID**: `9YIs0S7V11`
- **接入协议**: MQTT
- **设备类型**: 网关 + 子设备

### 属性列表

| 属性名 | 标识符 | 类型 | 说明 |
|--------|--------|------|------|
| 车位状态 | ParkStatus | int32 | 0=空闲, 1=有车, 2=僵尸车 |
| 地磁检测 | GeoMagnetic | int32 | 0=未检测到, 1=检测到车辆 |
| 超声波 | Ultrasonic | int32 | 超声波距离 (cm) |
| 占用时间 | OccupiedTime | int32 | 占用时长 (秒) |
| LED 状态 | LED | bool | LED 灯实际状态 |
| LED 使能 | LedEnable | bool | LED 控制开关 |

## 常见问题

### Q: 连接设备失败？
1. 确认 USB 调试已开启
2. 运行 `flutter devices` 检查设备是否识别
3. 检查 `android/app/build.gradle` 的 minSdkVersion 是否 >= 21

### Q: API 请求失败？
1. 检查 `_apiKey` 是否正确
2. 确认设备 ID 和产品 ID 配置正确
3. 设备是否在线（可通过 OneNET 控制台检查）

### Q: 如何添加更多页面？
```dart
// 1. 在 lib/pages/ 创建新页面
// 2. 在 main.dart 的 routes 中注册
MaterialApp(
  routes: {
    '/your-page': (context) => YourPage(),
  },
)

// 3. 使用 Navigator 跳转
Navigator.pushNamed(context, '/your-page');
```

## 开发环境要求

- Flutter SDK >= 3.10.0
- Dart SDK >= 3.0.0
- Android Studio / VS Code
- Android SDK (Android 开发)
- Xcode (iOS 开发, 仅 macOS)

## 许可证

MIT License
