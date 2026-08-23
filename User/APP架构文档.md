# 路边僵尸车检测系统 · APP 结构总结 & 可优化点清单

> **版本**：v2.0（5 Tab 架构）  
> **技术栈**：Flutter (Dart) + `http` + `fl_chart` + `crypto` + Material 3（华为「智慧生活」风高端科技感视觉）  
> **后端**：中国移动 OneNET 物联网平台（`https://iot-api.heclouds.com`），通过 REST + HMAC-MD5 签名鉴权  
> **项目结构**：features 分模块（页面级） + core（模型/服务/主题） + shared（公共组件）  
> **编码**：23 个 `.dart` 文件已全部严格通过 UTF-8 NoBOM 审计  
> **更新日期**：2026-08-20

---

## 一、页面层级 & 导航结构

```
MaterialApp（title: 路边僵尸车检测系统）
└── MainShell（Scaffold + 自定义底部导航栏 5 Tab）
    ├── [0] 总览  OverviewPage                ← Tab 首页
    ├── [1] 车位  SpotsPage                   ← Tab 车位
    │    └── SpotDetailPage                   ← push 子页（车位详情）
    ├── [2] 告警  AlertsPage                  ← Tab 告警（僵尸车工单）
    ├── [3] 数据  StatsPage                   ← Tab 数据复盘
    └── [4] 我的  ProfilePage                 ← Tab 个人中心
```

**路由实现**：`MainShell` 用 `IndexedStack` + `_currentIndex` 切 Tab（保留状态）；子页面走 `Navigator.push(MaterialPageRoute)`；`OverviewPage` 内还残留一处 `Navigator.pushNamed('/alerts')`（但未注册命名路由表，可能报错，见优化点）。

---

## 二、每个页面详细结构

### 🔴 Tab 0 · 总览页（OverviewPage）
代码：[overview_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/overview/presentation/pages/overview_page.dart)

| 区块 | 内容 | 数据来源 |
|---|---|---|
| **顶部栏** | 左：大标题「总览」28sp w700；右：铃铛图标（通知占位） | 本地 |
| **问候卡** | 左太阳图标 + 右「**早上好/下午好/晚上好**，管理员」+ `M月D日 · 周X`；分隔线下 3 项健康灯：LoRa网关 🟢 / 摄像头 🟢 / 服务器 🟢（全硬编码 true） | 本地 `DateTime.now()`；在线状态 **硬编码** |
| **车位概览卡** | 4 个数字横向排列：**总车位 / 空闲 / 占用 / 僵尸车**，中间细竖线分隔。点击「僵尸车」数字试图跳转 `/alerts` 命名路由 | `_spots` 本地统计 `.where().length` |
| **饼图卡**（fl_chart PieChart） | 左：饼图（空闲🟢 / 占用🟡 / 僵尸🔴）百分比写在扇区上；右：图例。底部蓝底提示条：「**B区最繁忙，占用率 XX%，建议引导车辆至其他区域**」 | `_spots` 本地计算（循环遍历 A/B/C 三区算占用率最高的分区——**这里还在算 C 区，虽然车位页已去掉 C Tab**，数据可能仍有 C 区设备但前端不展示） |
| **最新告警卡** | 左警告图标 + 标题「最新僵尸车告警」+ 右上「查看全部 →」按钮。下方红透底卡片：汽车图标 + 车牌号粗字 + 车位号+占用小时 + 右状态徽标。空状态显示「暂无僵尸车告警」。整张卡可点击 → 试图 pushNamed('/alerts') | `_alerts.first` |

**刷新**：`RefreshIndicator` → 重新调用 `getSpots()` + `getAlerts()`（**串行两个 await**，非并发）

---

### 🟡 Tab 1 · 车位页（SpotsPage）
代码：[spots_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/spots/presentation/pages/spots_page.dart)

| 区块 | 内容 | 数据来源 |
|---|---|---|
| 顶部栏 | 「车位」28sp w700 + 搜索图标 | 本地 |
| **分区 Tab**（CardContainer 包 Row+Expanded） | 2 项：**A区 / B区**（选中蓝底蓝字，未选中灰字）—— 已按要求删掉了 C 区 | `_selectedZone` state |
| **当前分区概览卡** | 4 列：总车位 / 空闲🟢 / 占用🟡 / 僵尸车🔴（与总览页统计项一致） | `_filteredSpots` where 过滤 |
| **车位地图卡** | 标题 + 右上「点击查看详情」小字；下方 **3 列 GridView**（`shrinkWrap=true`，`childAspectRatio=0.85`）每格圆角卡：顶部门牌号徽章（Park003 → 截 4 位 "rk003"？实际是 `length>6` 才截，`Park003`=7 会截掉 → "rk003" 这个显示错误）+ 状态文字 + 占用 h 数。颜色：绿/黄/红 | `_filteredSpots` = `_spots.where(s.zone==_selectedZone)` |
| **加载前过滤**（新加） | `abSpots = spots.where(A/B)` → 即使 API 回来 C 区车位也强制过滤掉 | 业务逻辑层 |

**点击车位** → `Navigator.push(MaterialPageRoute) → SpotDetailPage(spot: spot)`

---

### 🟠 Tab 1 子页 · 车位详情页（SpotDetailPage）
代码：[spot_detail_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/spots/presentation/pages/spot_detail_page.dart)

| 区块 | 内容 | 数据来源 |
|---|---|---|
| AppBar | `CustomAppBar`：`「A区 · Park003」` + 返回按钮 | widget.spot |
| **状态头卡** | 左：车位号 24sp w700；右：StatusBadge（free/occupied/zombie）。蓝底条 3 列：**电量%🔋（>50绿/<50红）/ 信号dBm📶 / 占用时长⏱（僵尸红）** | spot.batteryLevel / signalStrength / occupiedHours（**注意：这 3 个字段在 SpotModel.fromJson 里是硬编码 85.0 / -65，不来自真实 OneNET 属性！**） |
| **车辆信息卡** | 黄/绿 大色块（占用黄/空闲绿）+ 汽车/对勾大图标；下方 Surface 胶囊展示车牌号（22sp w700 letterSpacing:2）；底部文字「车辆已占用该车位 / 车位当前空闲」 | spot.plateNumber / spot.status |
| **设备信息卡** | label-value 行：设备名称 / 设备ID / 所属分区 / 最后更新 / 在线状态（硬编码「在线」） | `_deviceDetail = apiService.getDeviceDetail(spot.id)` → 失败走 mock |
| **快捷操作** | `ActionButtonGroup`：3 按钮 **派单 / 通知车主 / 处置** → 分别调用 `dispatchAlert('')` / `notifyOwner('')` / `resolveAlert('')`（**传空字符串 alertId，API 也是空实现 mock delay**）→ 底部 SnackBar 提示 | ApiService 假方法 + SnackBar |

---

### 🟣 Tab 2 · 告警管理页（AlertsPage / 僵尸车工单）
代码：[alerts_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/alerts/presentation/pages/alerts_page.dart)

按设计图重做后的结构：

| 区块 | 内容 |
|---|---|
| **顶部标题** | 居中「**告警管理**」28sp w700（**无蓝色导航栏/无返回按钮**——因为已是底部 Tab 主页，不是 push 进来的二级页） |
| **筛选 Tab** | 4 个圆角横向滚动 Pill：**全部（选中蓝底）/ 待处理 / 处理中 / 已处理**。点击切换 state `_selectedFilter`，自动过滤 Alerts |
| **告警卡片（×N）** | 方形图标 56×56（12 圆角，透底彩色 + 图标）+ 右侧文字列 + 右下两个按钮：<br> 🔴 待处理 → 红底⚠️ ；🟠 处理中 → 橙底↻ ；🟢 已处理 → 绿底✓<br> 右列首行：「**僵尸车告警 - {车牌号}**」粗字 + 右上 StatusBadge<br> 次行：「**车位: {spotId} · 占用 {hours}小时**」<br> 三行：「**创建时间: YYYY-M-D**」<br> 右下按钮：**删除🗑（红透底+垃圾桶，有二次确认）+ 处理✓（蓝底白字+白勾，按当前 status 推进：pending→dispatched→resolved）** |

**空状态**：EmptyState 组件（空列表提示）  
**加载**：LoadingIndicator  
**刷新**：RefreshIndicator

---

### 🔵 Tab 3 · 数据复盘页（StatsPage）
代码：[stats_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/stats/presentation/pages/stats_page.dart)

| 区块 | 内容 | 数据来源 |
|---|---|---|
| AppBar | `CustomAppBar`：「数据复盘」标题 | |
| 副标题 | 「数据统计概览」20sp w600 + 「截至 YYYY-MM-DD 的统计数据」小字 | DateTime.now() |
| **顶部概览双卡**（Row 两张 CardContainer 并排） | 左蓝：总车位（🅿️图标 + 大数字）；右黄：占用中（🚗图标 + 大数字） | `stats.totalSpots / stats.occupiedSpots` |
| **车位占用分析（饼图）** | 比总览页更大（180px 高），扇区 label 是中文「空闲/占用/僵尸车」而非百分比；右侧图例是具体百分比（1 位小数） | `stats.occupancyRate / zombieSpots` |
| **一周趋势（折线图）** | `LineChart`（isCurved 曲线）X 轴：「周一~周日」小字；Y 轴 0~100%。触点击中浮提示「周三: 60%」。下方橙底高亮卡「**本周告警总数 XX 起**」 | `stats.weeklyTrend`（**mock 写死 7 天数据**，不是真实后端历史统计） |

**API 失败兜底**：`StatsModel.empty()`（totalSpots=32 硬编码默认值）

---

### 🟤 Tab 4 · 我的（ProfilePage）
代码：[profile_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/profile/presentation/pages/profile_page.dart)

全部是静态 UI / 硬编码数据，未接 API：

| 区块 | 内容 |
|---|---|
| 顶部栏 | 「我的」28sp w700 + 铃铛通知图标 |
| **用户卡** | 左 56×56 蓝色圆形头像 👤 + 中「管理员 / 系统管理员 v2.0」 + 右 chevron。分隔线下 3 列统计：**128 处理工单 / 8 在线设备 / 99% 系统可用**（全硬编码） |
| **快捷操作**（4 格横向） | 🔴 告警中心 / 🔵 车位管理 / 🟡 数据导出 / 🟢 云端同步 → `onTap: () {}`（**空实现**） |
| **系统设置组卡** | ⚙️ 系统设置 / 📱 节点管理 / 📜 策略配置 → 空实现 |
| **运维组卡** | ⏳ 操作日志 / ⬆️ 固件升级 / 🐞 故障诊断 → 空实现 |
| **账户组卡** | 👤 个人资料 / 🔒 账号安全 / ❓ 帮助与反馈 / ℹ️ 关于我们 → 空实现 |

---

## 三、核心数据模型（3 个）

### 1. SpotModel（车位/节点设备）
代码：[spot_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/spot_model.dart)

```dart
id (设备名/Park001) / zone (A/B/C) / status (free/occupied/zombie)
occupiedHours (由 OccupiedTime秒 ÷3600 四舍五入)
batteryLevel ⚠️ (fromJson 硬编码 85.0，不从 OneNET 读)
signalStrength ⚠️ (fromJson 硬编码 -65，不从 OneNET 读)
plateNumber (来自 OneNET plate_number 属性)
lastUpdated
```

**逻辑要点**：
- `isFree / isOccupied / isZombie` 3 个 getter
- **分区映射 `_extractZone` 逻辑过时**：Park001/002 → A；003/004 → B；**其他 → C**（但车位页已强制只留 A/B，以后新增 Park005 若走默认会被判 C 被过滤掉，要专家确认分区策略）
- ParkStatus=1 或 Ultrasonic<30cm → occupied

### 2. AlertModel（僵尸车告警 / 工单）
代码：[alert_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/alert_model.dart)

```dart
id / plateNumber / spotId / occupiedHours / status (pending/dispatched/resolved)
imageUrl? / createdAt?
```

- `isPending / isDispatched / isResolved` getter
- 注意：**后端没有独立「告警」接口**，完全由 `ApiService.getAlerts()` 本地合成：遍历所有车位 → occupiedHours ≥ 24h 就算告警、≥72h = pending 否则 dispatched

### 3. StatsModel（复盘数据）
代码：[stats_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/stats_model.dart)

```dart
totalSpots / occupiedSpots / zombieSpots / occupancyRate
weeklyTrend: List<DailyTrend> (date / avgOccupancy% / alertsCount)
```

- 提供 `.empty()` 兜底（totalSpots 硬编码=32）
- `occupancyRate` 语义：**占用率（只算 occupied，不含 zombie）**—— 这会导致 `freeRate = 1 - occupancyRate - zombieRate`，如果占用 + 僵尸 > 100% 会负数（一般不会，但专家可以确认语义是否想合并 zombie 到 occupied）

---

## 四、API 服务（OneNET 对接层）
代码：[api_service.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/services/api_service.dart)

### ⚠️ 安全 + 正确性重大发现（专家重点看）
```dart
// 直接硬编码在源码里！
static const String _accessKey = 'e3b97243b0d24ffda1befead601ef617';
```
**AccessKey（OneNET 主鉴权密钥）明文硬编码在 ApiService 静态 const 里**——反编译 APK 即可直接取出，等于给所有安装用户 OneNET 账户的完全权限。这是最大架构问题。

| 方法 | 真实接口？ | 备注 |
|---|---|---|
| `getSpots()` | ⚠️ **部分真实** | 先调用 `/device/list`（两个 product_id：网关 + 节点）→ 再循环 `/thingmodel/query-device-property` **逐个查属性（串行 for 循环！N 个设备 = N 次串行 HTTP）** → 空/失败 → 返回 Mock 6 个车位 |
| `_generateAuthorization()` | ❌ **没正确实现** | 返回字符串是硬编码的模板 `'version=&res=&et=&method=&sign='`，完全没把 version/res/et/method/signature 拼进去，变量都算完了但根本没用！所以所有签名请求发到 OneNET 一定失败，→ 也就是说**当前 APP 一直在走 Mock 分支，没有真正读 OneNET 数据**。这是目前最大正确性 Bug。 |
| `getDeviceDetail()` | ⚠️ 同上 | 签名一样无效，走 mock |
| `setProperty()` | ⚠️ 同上 | 签名无效，catch 里 `return true`（**假装设置成功**） |
| `getAlerts()` | ❌ 本地合成 | 调 `getSpots()` → 遍历车位，≥24h 占用 → new AlertModel。无后端持久化：派单/删除/处理 全是本地内存 setState，热重启就丢失 |
| `dispatchAlert/notifyOwner/resolveAlert` | ❌ 纯 Mock | `Future.delayed(1s) return true` |
| `getStats()` | ❌ 半合成 | `total/occupied/zombie/occupancyRate` 来自 getSpots 本地统计；`weeklyTrend` **完全 mock 写死（周一到周日固定 7 条）**。后端没历史统计接口 |

### 4.1 OneNET 平台接口列表（官方参考）

> 来源：[云平台接口列表](https://open.iot.10086.cn/doc/aiot/fuse/detail/1483)（最近更新时间：2026-05-29）
> 本 App 实际使用到的接口已用 **加粗** 标注；其余为平台能力清单，供后续扩展参考。

| 分类 | URL | 接口名称 | 本项目使用 |
|---|---|---|---|
| 产品管理 | /product/detail | 产品详情 | |
| 设备管理 | /device/create | 创建设备 | |
| 设备管理 | **/device/list** | **设备列表查询** | ✅ 读取全部车位设备（含离线） |
| 设备管理 | /device/detail | 设备详情 | |
| 设备管理 | /device/update | 更新设备 | |
| 设备管理 | /device/delete | 删除设备 | |
| 设备管理 | /device/reset-seckey | 重置设备接入鉴权key | |
| 设备管理 | /device/status-history | 设备状态历史变更记录 | |
| 设备管理 | /device/operation-log | 设备操作记录查询 | |
| 设备管理 | /device/service-log | 设备服务记录查询 | |
| 设备管理 | /device/event-log | 设备事件记录查询 | |
| 设备管理 | /device/movedevice | 设备转移 | |
| 设备文件管理 | /device/file-list | 账户文件列表查询 | |
| 设备文件管理 | /device/file-upload | 设备文件上传 | |
| 设备文件管理 | /device/file-delete | 设备文件删除 | |
| 设备文件管理 | /device/file-download | 设备文件下载 | |
| 设备文件管理 | /device/file-space | 账户文件存储空间查询 | |
| 设备文件管理 | /device/file-device-count | 设备文件数量查询 | |
| 物模型管理 | /thingmodel/query-system-thing-model | 物模型系统功能点列表 | |
| 物模型管理 | /thingmodel/query-thing-model | 物模型查询 | |
| 物模型使用 | /thingmodel/set-device-property | 设置设备属性 | ✅ 下发 LED/LedEnable 控制 |
| 物模型使用 | /thingmodel/query-device-property-detail | 获取设备属性详情 | |
| 物模型使用 | **/thingmodel/query-device-property** | **设备属性最新数据查询** | ✅ 读取 ParkStatus/Ultrasonic 等 |
| 物模型使用 | /thingmodel/query-device-property-history | 设备属性记录查询 | |
| 物模型使用 | /thingmodel/call-service | 设备服务调用 | |
| 物模型使用 | /thingmodel/set-device-desired-property | 设备属性期望设置 | |
| 物模型使用 | /thingmodel/query-device-desired-property | 设备属性期望查询 | |
| 物模型使用 | /thingmodel/delete-device-desired-property | 设备属性期望删除 | |
| 数据流使用 | /datapoint/history-datapoints | 查询设备数据点 | |
| 数据流使用 | /datapoint/current-datapoints | 批量查询产品下设备最新数据点 | |
| 命令下发 | /datapoint/synccmds | 设备下发命令 | |
| LwM2M-即时命令 | /nb-iot/discover | 即时命令-资源发现 | |
| LwM2M-即时命令 | /nb-iot/observe | 即时命令-资源订阅 | |
| LwM2M-即时命令 | /nb-iot | 即时命令-读取设备资源 | |
| LwM2M-即时命令 | /nb-iot | 即时命令-写入设备资源 | |
| LwM2M-即时命令 | /nb-iot/execute | 即时命令-设备命令下发 | |
| LwM2M-缓存命令 | /nb-iot/offline | 缓存命令-读取设备资源 | |
| LwM2M-缓存命令 | /nb-iot/offline | 缓存命令-写入设备资源 | |
| LwM2M-缓存命令 | /nb-iot/execute/offline | 缓存命令-设备命令下发 | |
| LwM2M-缓存命令 | /nb-iot/offline/history | 缓存命令-查询指定设备缓存命令列表 | |
| LwM2M-缓存命令 | /nb-iot/offline/history/:uuid | 缓存命令-查询指定缓存命令详情 | |
| LwM2M-缓存命令 | /nb-iot/offline/cancel/:uuid | 缓存命令-取消指定的缓存命令 | |
| LwM2M-缓存命令 | /nb-iot/offline/cancel/all | 缓存命令-取消设备所有未下发的缓存命令 | |
| LwM2M-缓存命令 | /nb-iot/offline/history/:uuid/piecewise | 缓存命令-全链路日志查询 | |
| LwM2M-DTLS | /nb-iot/device/psk | 查看指定设备bs_psk信息 | |
| LwM2M-DTLS | /nb-iot/device/psk | 更新指定设备bs_psk信息 | |
| LwM2M-DTLS | /nb-iot/device/accpsk | 查看指定设备acc_psk信息 | |
| LwM2M-DTLS | /nb-iot/device/accpsk | 新增指定设备acc_psk信息 | |
| LwM2M-DTLS | /nb-iot/device/accpsk | 编辑指定设备acc_psk信息 | |
| 工业标识管理 | /fuse-identity-device/batch-auto-regist-device-identity | 设备批量自动注册标识接口 | |
| LBS位置能力 | /fuse-lbs/latest-location | 基站定位获取最新位置接口 | |
| LBS位置能力 | /fuse-lbs/get-trail | 基站定位历史轨迹查询接口 | |
| LBS位置能力 | /fuse-lbs/latest-wifi-location | WIFI定位获取最新位置接口 | |
| LBS位置能力 | /fuse-lbs/get-wifi-trail | WIFI定位历史轨迹查询接口 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/check | 检测升级任务 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/{tid}/download | 下载升级包 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/{tid}/status | 上报升级状态 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/{tid}/check | 检测任务状态 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/version | 查看设备版本号 | |
| OTA南向 | /fuse-ota/{pro_id}/{dev_name}/version | 上报版本号 | |
| 语音能力 | /fuse-voice/voiceNotify | 语音通知接口 | |
| 应用开发 | /project/summary | 项目概况 | |
| 应用开发 | /project/product-list | 项目集成产品列表 | |
| 应用开发 | /project/device-list | 项目集成设备列表 | |
| 应用开发 | /project/add-device | 项目添加设备 | |
| 应用开发 | /project/remove-device | 项目移除设备 | |
| 智能体调用 | /agent/ai-esim/api/stream/ | WebSocket API | |
| 智能体调用 | /agents/device/timbre/list | 音色查询 | |
| 智能体调用 | /agents/device/role/add | 新增设备自定义角色 | |
| 智能体调用 | /agents/device/role/update | 修改设备自定义角色 | |
| 智能体调用 | /agents/device/role/delete | 删除设备自定义角色 | |
| 智能体调用 | /agents/device/role/reset | 配置设备预设角色 | |
| 智能体调用 | /agents/device/role/config | 配置设备自定义角色 | |
| 智能体调用 | /agents/device/role/list | 查询设备角色列表 | |
| 智能体调用 | /agents/device/role/current | 查询设备当前角色 | |
| 智能体调用 | /agents/device/user/add | 添加设备用户信息 | |
| 智能体调用 | /agents/device/user/update | 修改设备用户信息 | |
| 智能体调用 | /agents/device/user/info | 查询设备用户信息 | |
| 智能体调用 | /agents/device/user/delete | 清除设备用户信息 | |
| 智能体调用 | /agents/device/unbind | 设备智能体解绑重置 | |
| 机卡协同 | /machine-card/batch-add-sim | 批量添加SIM卡 | |
| 机卡协同 | /machine-card/bind | 单个添加机卡关联 | |
| 机卡协同 | /machine-card/batch-bind | 批量添加机卡关联 | |
| 机卡协同 | /machine-card/update-bind | 修改机卡关联 | |
| 机卡协同 | /machine-card/unbind | 单个删除机卡关联 | |
| 机卡协同 | /machine-card/batch-unbind | 批量删除机卡关联 | |
| 机卡协同 | /machine-card/query-card-info | 查询指定设备的机卡信息 | |
| 机卡协同 | /machine-card/query-card-margin | 查询指定设备的套餐和用量 | |
| 机卡协同 | /machine-card/query-batch-status-info | 批次任务状态查询 | |
| 机卡协同 | /machine-card/verify | 机卡绑定校验 | |

---

## 五、公共组件 & 主题（可复用度）

| 文件 | 作用 | 备注 |
|---|---|---|
| `theme/app_colors.dart` | 配色变量（primary/success/warning/danger 四色体系） | 有 |
| `theme/app_dims.dart` | 间距/圆角常量（gapCard/radiusMedium/paddingPage…） | 有，UI 一致性较好 |
| `widgets/card_container.dart` | 所有卡片容器（圆角 + Material3 阴影） | 全项目复用率高，是基石组件 |
| `widgets/status_badge.dart` | pending/dispatched/resolved + free/occupied/zombie 6 种徽标 | 复用率高 |
| `widgets/custom_app_bar.dart` | 二级页通用蓝色渐变顶栏 + 返回按钮 | 只在二级页用（stats/spot_detail），5 个 Tab 主页都是直接做大标题 header |
| `widgets/action_button_group.dart` | 派单/通知/处置 3 按钮横向排列 | 仅 spot_detail 用 |
| `widgets/circle_progress.dart` | 环形进度图 | 暂未看到调用，可能是旧代码 |
| `widgets/huawei_card.dart` | 华为风卡片基类 | 暂未看到调用 |
| `widgets/empty_state.dart` | 空状态占位图 | alerts_page 用 |
| `widgets/loading_indicator.dart` | 全屏加载提示 | stats_page 用 |
| `widgets/stat_card.dart` | 统计卡（概览页那种） | 暂未看到直接使用，可能被本地 _buildStatItem 覆盖了 |
| `widgets/list_tile_base.dart` | 列表行基类 | profile 页没用，自己重新实现了 group item |

---

# 六、专家评审「可优化点清单」（按优先级 × 分类）

## 🔴 🔴 优先级 P0 · 不解决就不能上线的致命问题

### 1. OneNET 鉴权签名完全无效，APP 一直在读 Mock 数据
`_generateAuthorization()` 函数计算了 version/res/et/method/hmac-md5 签名，但最终返回的字符串是硬编码空模板 `version=&res=&et=&method=&sign=`，所有变量一个都没拼进去。**请求到 OneNET 一定返回非 0 code 或 401，立刻走 catch → 返回 Mock 数据**。这就意味着：**APP 现在能看到的车位/告警数据，全是本地写死的那 6 个 Park001~006 样板 + 3 条 alert 样板**，跟真实硬件、真实 OneNET 后台完全没连上。

👉 **建议**：按 OneNET 2020-05-29 版鉴权文档正确拼接 `version=2020-05-29&res=userid%2F528332&et=...&method=md5&sign=...` 完整 Authorization 头，签名 `stringForSignature` 也不是 `\n\n\n`，需要包含 HTTP method/URI/query 等参数（查 OneNET 文档对应章节）。

### 2. AccessKey 明文硬编码在源码里，严重安全漏洞
`_accessKey = 'e3b97243b0d24ffda1befead601ef617'` 写在静态 const，反编译 APK 秒提取。拥有这个 Key 的人可以随意读写所有设备属性、删除设备、下发 OTA…… 等于把 OneNET 账户拱手送出。

👉 **建议**：永远不要在移动端直接调用 OneNET 管理级 API。改为：
- 新增一个你自己的后端服务（比如 Python/FastAPI/Node），Mobile → HTTPS → 你的 Backend → 内部鉴权 → OneNET API
- 后端保管 AccessKey（环境变量/KMS），前端只拿一个用户级 JWT
- 移动端加 SSL Pinning，防抓包中间人

### 3. `Navigator.pushNamed('/alerts')` 无路由表，运行时必然报错
[overview_page.dart 第 260 行](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/overview/presentation/pages/overview_page.dart#L257-L264)：点击「僵尸车」数字 / 最新告警整张卡 → pushNamed('/alerts')。但 [main.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/main.dart) 里 `MaterialApp` 根本没有 `routes:` / `onGenerateRoute` 参数。一点击就是 `Could not find a generator for route RouteSettings("/alerts", null) in the _WidgetsAppState` 红屏。

👉 **建议**：改成和底部导航联动（`MainShell` 接收全局 key 或通过 context 找到 `_MainShellState` 调 `setState(() => _currentIndex = 2)` 切 Tab），或者干脆注册命名路由表。

---

## 🟠 🔴 优先级 P1 · 重大功能 / 体验问题

### 4. Alerts（工单）全是本地状态，没有任何持久化/后端同步
`dispatchAlert / resolveAlert / deleteAlert` 全是 `setState` 改本地 `_alerts` 列表 + ApiService 里 `Future.delayed(1s) return true` 空实现。刷新下拉 / 切 Tab / 杀进程，所有处理状态丢失。

👉 **建议**：要么 OneNET 侧建一个「告警工单」Data Stream，要么自建工单表后端。同时 Alerts 生成逻辑也要从「本地从车位合成」改成「后端服务定时扫描 + 推工单」——因为 72h 僵尸车判定本身就是后端任务，不能靠 APP 前台打开了才算。

### 5. getSpots 逐个串行查属性，N 个设备 = N 次串行 HTTP（极慢）
[_fetchDeviceProperties](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/services/api_service.dart#L86-L146)：`for (final device in devices) { await _client.get(...); }` 串行 for-await。20 个设备 → 20 次 HTTP 串行，加上 RTT，首屏可能要 10~20 秒才能出数据。用户体验极差。

👉 **建议**：
- 改成 `Future.wait(devices.map((d) => _fetchSingle(d)))` 并发（要注意限流防止 OneNET 限频）
- 或用 OneNET 的「批量查询设备属性」接口（如果支持）
- 或本地 Hive 缓存上次属性，先显示缓存再后台增量刷新（skeleton screen）

### 6. SpotModel.batteryLevel / signalStrength 硬编码假数据
`fromJson` 里直接写死 `batteryLevel: 85.0` / `signalStrength: -65`，UI 上却显示得像真实数据（绿/红电量图标、dBm 数字），运维人员会被误导。`_buildHealthItem`（LoRa网关/摄像头/服务器 绿点）也是硬编码 `true`。

👉 **建议**：如果 OneNET 模型里有 Battery/Signal 字段就从 properties 里取；如果没有就先把 UI 改成「— / 未知」样式，或者标明「演示数据」。

### 7. Overview 饼图 + Stats 一周趋势「最繁忙分区/折线」数据假
- 概览页「最繁忙分区」循环 `['A','B','C']`（**还在算 C 区**，与车位页只留 A/B 矛盾，而且 C 区车位被前端过滤后 A/B 区率都等于 0%，所以可能永远显示 A 区）
- Stats 一周趋势完全来自 `_getMockWeeklyTrend()` 写死的 7 条，永远是周一 45%…

👉 **建议**：要么加后端历史统计表；要么先把「一周趋势」改成占位显示「暂无历史数据」+ 顶部提示即将上线，别用假数据骗专家/用户。

### 8. ActionButtonGroup 在 SpotDetailPage 传 alertId 是空字符串
`dispatchAlert('') / notifyOwner('') / resolveAlert('')` 三个 API 调用参数是空。以后真接后端，派单根本不知道派的是哪辆车。

👉 **建议**：把 spot → 对应 alert 的 id 逻辑先写好（或 action 改以 deviceId 为维度，因为 spot_detail 本身就知道是哪个车位）。

---

## 🟡 🟡 优先级 P2 · UI / 一致性 / 易维护性问题

### 9. 首页 vs 其他页顶栏结构不统一
- 4 个 Tab 主页（Overview / Spots / Alerts / Profile）：直接用 Padding + `MediaQuery.padding.top`，做大标题 + 右侧图标，风格一致
- 但是 **StatsPage**：用了 `CustomAppBar(title: '数据复盘')`（蓝色渐变顶栏，比其他 4 个主页风格更像二级页）
- **SpotDetailPage**：也用 CustomAppBar，这个合理（二级页）

👉 **建议**：StatsPage 既然是 5 Tab 之一的主页，把 `appBar: CustomAppBar` 去掉，改成和其他 4 主页一样的「大标题结构」，视觉一致性更高。

### 10. ProfilePage 快捷操作 / 分组全部 onTap 为空
共 4（快捷）+ 3（系统）+ 3（运维）+ 4（账户）= **14 个点了无任何反馈的死入口**，会让用户困惑。

👉 **建议**：要么全部替换为 `SnackBar("功能开发中")` 提示；要么把还没实现的分组先收起来，只放已实现的 2~3 个。

### 11. Grid 车位号截取有 bug
`spot.id.length > 6 ? spot.id.substring(spot.id.length - 4) : spot.id`
- 「Park001」长度是 7，触发截取 → 结果是 substring(3) = "rk001"，**把 P/ar 丢掉只留尾巴**，很奇怪。

👉 **建议**：直接取整个 id 或者按 `Park### → A##` 的格式做专门格式化逻辑。

### 12. Profile 用户卡"128 处理工单 / 8 在线设备 / 99% 系统可用"全是硬编码
用户会当真。至少加个「演示数据」灰色小字标签或者从 API/常量动态取。

### 13. 错误处理太"安静"，Debug 很难
`_loadSpots()` / `_loadData()` 里的 `catch(e)` 只做了 `setState(() => isLoading = false)`，**把 exception 完全吞了**。永远不知道是签名错了、网络挂了、还是 OneNET 限流。

👉 **建议**：`catch (e, stackTrace) { debugPrint('$e\n$stackTrace');` 打日志 + 页面底部用 SnackBar/ErrorWidget 提示用户「加载失败，请下拉重试」。

### 14. `Navigator.of(context).push(MaterialPageRoute)` 有 2 处、`pushNamed` 有 2 处，路由分散
以后加登录页/权限判断会很痛苦。

👉 **建议**：统一改成 `go_router`（官方推荐）或至少注册一份集中的 `routes: {}` 表，所有跳转通过静态常量路由名（例如 `AppRoutes.spotDetail`）。

### 15. 状态管理全靠 `setState` + 跨页面重复拉取数据
每个页面（Overview / Spots / Alerts / Stats）**各自在 `initState` 里独立调 `ApiService().getSpots()`**，同一份车位数据首页拉一次、车位页再拉一次、告警页再拉一次（间接）、数据页再拉一次（间接）。5 Tab 切一遍 = 同一个接口至少拉 4 遍。

👉 **建议**：
- 用 `ChangeNotifier + Provider`（最小改动）做一个 `ParkingRepository`，里面存 `List<SpotModel>` 加 `lastFetchAt` 缓存（比如 60s 内不重复请求）
- 所有页面从同一个 Provider 读数据，首次进入 + 下拉刷新才重新拉
- 再进一步可以接 `Riverpod` 2.0 / `Bloc`（如果团队熟悉）

---

## 🟢 优先级 P3 · 性能 / 细节优化

### 16. `GridView(shrinkWrap: true + NeverScrollableScrollPhysics)` + `SingleChildScrollView` 性能隐患
所有页面（Overview / Spots / Stats / Profile）都是套了 `SingleChildScrollView`，内部 `GridView.builder` 用了 `shrinkWrap: true`。shrinkWrap 会一次性 build 所有子节点，车位如果从 6 个扩到 200 个（真停车场规模）直接卡顿 + 丢帧。

👉 **建议**：改用 `CustomScrollView + SliverGrid`，或者 `NestedScrollView`，保留懒加载。

### 17. 所有页面底部 `SizedBox(height: 80/100)` 硬编码给底部导航留位置
靠经验值会在不同屏幕高度 / 字号缩放（Accessibility Large Text）时出现遮挡。

👉 **建议**：用 `SafeArea(bottom: true)` 或 `SliverSafeArea`，或用 `LayoutBuilder` 取底部 padding。

### 18. `ImageUrl` 字段定义了但 UI 没用到
告警卡车牌没有对应车辆抓拍图（`imageUrl` 在 AlertModel 里，但 alerts_page.dart 卡片构建里完全没展示）。真实车牌识别系统的告警一般必须有图作为证据，这是「工单可运维性」关键缺失。

### 19. 编码规范 & 格式化 & 静态分析
`analysis_options.yaml` 已存在（项目里 User/analysis_options.yaml）但没跑 lint。建议 CI 流水线里加 `flutter analyze --no-fatal-infos` 卡点 + `dart format --set-exit-if-changed .` 防止个人风格乱。

### 20. 单元测试 / Widget 测试缺失
**全项目 0 个 test 目录/文件**。对于物联网（有明确硬件状态机：ParkStatus 0/1 + Ultrasonic + OccupiedTime）的项目，SpotModel.fromJson、AlertModel 判定逻辑（24h/72h 阈值）、分区映射这些都很适合单元测试覆盖，防止以后改逻辑改崩。

---

## 🟦 加分项 / 长期方向（可选）
- **真推送**：OneNET 侧告警 → 你自建后端 → 极光/个推/FCM 推 APNS/小米/华为通道到 APP，不用每次进告警页才刷
- **国际化 i10n**：现在所有中文硬编码在 Widget build 里，要给外省客户/英文客户要全面替换
- **无障碍**：`Semantics` 标注 / `Tooltip` / 大字体缩放适配（已经发现了底部 80 硬编码要崩）
- **离线模式**：Hive/isar 缓存最新车位快照，地下室断网也能看最后一次数据
- **工单操作审计流水**：谁什么时候改了哪个工单从 pending→resolved，后端一张表 + Profile 页「操作日志」对应真数据

---

## 附：关键文件索引（直接点击查看源码）

| 类型 | 文件 |
|---|---|
| 入口&导航 | [main.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/main.dart) |
| Tab0 总览 | [overview_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/overview/presentation/pages/overview_page.dart) |
| Tab1 车位 | [spots_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/spots/presentation/pages/spots_page.dart) |
| Tab1 子页车位详情 | [spot_detail_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/spots/presentation/pages/spot_detail_page.dart) |
| Tab2 告警 | [alerts_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/alerts/presentation/pages/alerts_page.dart) |
| Tab3 数据复盘 | [stats_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/stats/presentation/pages/stats_page.dart) |
| Tab4 我的 | [profile_page.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/features/profile/presentation/pages/profile_page.dart) |
| 模型 Spot | [spot_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/spot_model.dart) |
| 模型 Alert | [alert_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/alert_model.dart) |
| 模型 Stats | [stats_model.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/models/stats_model.dart) |
| API 服务（最关键） | [api_service.dart](file:///g:/All_Project/Keil_Project/STM32_Project/我的/智能停车场/User/lib/core/services/api_service.dart) |
| 主题 & 公共组件根目录 | `lib/core/theme/*` + `lib/shared/widgets/*` |
