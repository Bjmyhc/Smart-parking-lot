import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'core/theme/app_colors.dart';
import 'core/providers/parking_provider.dart';
import 'features/overview/presentation/pages/overview_page.dart';
import 'features/spots/presentation/pages/spots_page.dart';
import 'features/alerts/presentation/pages/alerts_page.dart';
import 'features/stats/presentation/pages/stats_page.dart';
import 'features/profile/presentation/pages/profile_page.dart';
import 'features/profile/presentation/widgets/ota_upgrade_dialog.dart';

void main() {
  runApp(const ParkingApp());
}

class ParkingApp extends StatelessWidget {
  const ParkingApp({super.key});

  @override
  Widget build(BuildContext context) {
    // 全局数据源: 唯一的车位数据/轮询/模式, 所有页面统一从这里读取
    return ChangeNotifierProvider(
      create: (_) => ParkingProvider(),
      child: MaterialApp(
      title: '路边僵尸车检测系统',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: AppColors.primary,
          primary: AppColors.primary,
          surface: AppColors.surface,
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: AppColors.background,
        appBarTheme: const AppBarTheme(
          backgroundColor: AppColors.surface,
          elevation: 0,
          centerTitle: true,
          titleTextStyle: TextStyle(
            fontSize: 18,
            fontWeight: FontWeight.w600,
            color: AppColors.textPrimary,
          ),
        ),
        bottomNavigationBarTheme: const BottomNavigationBarThemeData(
          selectedItemColor: AppColors.primary,
          unselectedItemColor: AppColors.textSecondary,
          backgroundColor: AppColors.surface,
          elevation: 8,
          type: BottomNavigationBarType.fixed,
        ),
      ),
      home: const MainShell(),
      routes: {
        '/alerts': (context) => const AlertsPage(),
      },
    ),
    );
  }
}

class MainShell extends StatefulWidget {
  const MainShell({super.key});

  @override
  State<MainShell> createState() => _MainShellState();
}

class _MainShellState extends State<MainShell> with WidgetsBindingObserver {
  int _currentIndex = 0;
  bool _otaDialogShowing = false;

  final List<Widget> _pages = const [
    OverviewPage(),
    SpotsPage(),
    AlertsPage(),
    StatsPage(),
    ProfilePage(),
  ];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // App 首次进入: 触发一轮 OTA 短检测
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<ParkingProvider>().beginOtaCheckSession();
    });
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // 每次回到前台: 重新触发一轮 OTA 短检测(低频, 不常驻轮询)
    if (state == AppLifecycleState.resumed) {
      context.read<ParkingProvider>().beginOtaCheckSession();
    }
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<ParkingProvider>();

    // OTA 升级弹窗: 检测到待升级任务(未被忽略)时全局弹出
    if (provider.otaPromptVisible && !_otaDialogShowing) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted || _otaDialogShowing || !provider.otaPromptVisible) return;
        _otaDialogShowing = true;
        showDialog<void>(
          context: context,
          barrierDismissible: false,
          builder: (_) => OtaUpgradeDialog(provider: provider),
        ).whenComplete(() {
          _otaDialogShowing = false;
          // 兜底: 若对话框被系统返回键关闭且未触发任何动作, 清掉提示位避免反复弹
          if (provider.otaPromptVisible) provider.dismissOtaPrompt();
        });
      });
    }

    return Scaffold(
      body: IndexedStack(
        index: _currentIndex,
        children: _pages,
      ),
      bottomNavigationBar: Container(
        decoration: BoxDecoration(
          color: Colors.white,
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.05),
              blurRadius: 10,
              offset: const Offset(0, -2),
            ),
          ],
        ),
        child: SafeArea(
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
            child: Row(
              children: [
                Expanded(child: _buildNavItem(0, Icons.home_outlined, Icons.home, '首页')),
                Expanded(child: _buildNavItem(1, Icons.local_parking_outlined, Icons.local_parking, '车位')),
                Expanded(child: _buildNavItem(2, Icons.warning_amber_outlined, Icons.warning_amber, '告警')),
                Expanded(child: _buildNavItem(3, Icons.bar_chart_outlined, Icons.bar_chart, '数据')),
                Expanded(child: _buildNavItem(4, Icons.person_outline, Icons.person, '我的')),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildNavItem(int index, IconData icon, IconData activeIcon, String label) {
    final isSelected = _currentIndex == index;
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onTap: () {
        setState(() {
          _currentIndex = index;
        });
      },
      child: SizedBox(
        width: double.infinity,
        child: Padding(
          padding: const EdgeInsets.symmetric(vertical: 8),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              AnimatedSwitcher(
                duration: const Duration(milliseconds: 200),
                transitionBuilder: (child, anim) =>
                    ScaleTransition(scale: anim, child: child),
                child: Icon(
                  isSelected ? activeIcon : icon,
                  key: ValueKey(isSelected),
                  color: isSelected ? AppColors.primary : AppColors.textSecondary,
                  size: 24,
                ),
              ),
              const SizedBox(height: 4),
              Text(
                label,
                style: TextStyle(
                  fontSize: 12,
                  fontWeight: isSelected ? FontWeight.w600 : FontWeight.normal,
                  color: isSelected ? AppColors.primary : AppColors.textSecondary,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
