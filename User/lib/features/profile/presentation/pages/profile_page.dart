import 'package:flutter/material.dart';
import '../../../../shared/widgets/card_container.dart';
import '../../../../core/theme/app_colors.dart';
import '../../../../core/theme/app_dims.dart';
import 'firmware_upgrade_page.dart';

class ProfilePage extends StatelessWidget {
  const ProfilePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      body: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _buildHeader(context),
            const SizedBox(height: 16),
            Padding(
              padding: const EdgeInsets.fromLTRB(
                AppDims.paddingPage,
                0,
                AppDims.paddingPage,
                AppDims.paddingPage,
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  _buildUserCard(),
                  const SizedBox(height: 12),
                  _buildSystemGroup(),
                  const SizedBox(height: 12),
                  _buildDeviceGroup(context),
                  const SizedBox(height: 12),
                  _buildAccountGroup(),
                  const SizedBox(height: 80),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildHeader(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16,
        left: AppDims.paddingPage,
        right: AppDims.paddingPage,
      ),
      child: Row(
        children: [
          const Expanded(
            child: Padding(
              padding: EdgeInsets.only(left: 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    '我的',
                    style: TextStyle(
                      fontSize: 28,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                ],
              ),
            ),
          ),
          Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              color: AppColors.surface,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Icon(Icons.notifications_none, color: AppColors.textSecondary, size: 22),
          ),
        ],
      ),
    );
  }

  Widget _buildUserCard() {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 16),
      child: Column(
        children: [
          Row(
            children: [
              Container(
                width: 56,
                height: 56,
                decoration: BoxDecoration(
                  color: AppColors.primary,
                  borderRadius: BorderRadius.circular(28),
                ),
                child: const Icon(Icons.person, color: Colors.white, size: 28),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Text(
                      '管理员',
                      style: TextStyle(
                        fontSize: 17,
                        fontWeight: FontWeight.w600,
                        color: AppColors.textPrimary,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      '系统管理员 · v2.0',
                      style: TextStyle(
                        fontSize: 13,
                        color: AppColors.textSecondary.withValues(alpha: 0.7),
                      ),
                    ),
                  ],
                ),
              ),
              const Icon(Icons.chevron_right, color: AppColors.textSecondary),
            ],
          ),
          const SizedBox(height: 16),
          Container(height: 1, color: AppColors.textSecondary.withValues(alpha: 0.1)),
          const SizedBox(height: 16),
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceAround,
            children: [
              _buildStatColumn('128', '处理工单'),
              Container(width: 1, height: 30, color: AppColors.textSecondary.withValues(alpha: 0.1)),
              _buildStatColumn('8', '在线设备'),
              Container(width: 1, height: 30, color: AppColors.textSecondary.withValues(alpha: 0.1)),
              _buildStatColumn('99%', '系统可用'),
            ],
          ),
        ],
      ),
    );
  }

  Widget _buildStatColumn(String value, String label) {
    return Expanded(
      child: Column(
        children: [
          Text(
            value,
            style: const TextStyle(
              fontSize: 18,
              fontWeight: FontWeight.w700,
              color: AppColors.textPrimary,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            label,
            style: const TextStyle(
              fontSize: 12,
              color: AppColors.textSecondary,
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildSystemGroup() {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Column(
        children: [
          _buildGroupItem(
            icon: Icons.settings_outlined,
            title: '系统设置',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.devices_outlined,
            title: '节点管理',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.policy_outlined,
            title: '策略配置',
          ),
        ],
      ),
    );
  }

  Widget _buildDeviceGroup(BuildContext context) {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Column(
        children: [
          _buildGroupItem(
            icon: Icons.history,
            title: '操作日志',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.update_outlined,
            title: '固件升级',
            onTap: () {
              Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const FirmwareUpgradePage()),
              );
            },
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.bug_report_outlined,
            title: '故障诊断',
          ),
        ],
      ),
    );
  }

  Widget _buildAccountGroup() {
    return CardContainer(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: Column(
        children: [
          _buildGroupItem(
            icon: Icons.person_outline,
            title: '个人资料',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.lock_outline,
            title: '账号安全',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.help_outline,
            title: '帮助与反馈',
          ),
          _buildDivider(),
          _buildGroupItem(
            icon: Icons.info_outline,
            title: '关于我们',
          ),
        ],
      ),
    );
  }

  Widget _buildGroupItem({required IconData icon, required String title, VoidCallback? onTap}) {
    return GestureDetector(
      onTap: onTap,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
        child: Row(
          children: [
            Icon(icon, color: AppColors.textPrimary.withValues(alpha: 0.7), size: 22),
            const SizedBox(width: 14),
            Expanded(
              child: Text(
                title,
                style: const TextStyle(
                  fontSize: 15,
                  color: AppColors.textPrimary,
                ),
              ),
            ),
            const Icon(Icons.chevron_right, color: AppColors.textSecondary, size: 20),
          ],
        ),
      ),
    );
  }

  Widget _buildDivider() {
    return Padding(
      padding: const EdgeInsets.only(left: 50, right: 20),
      child: Container(height: 1, color: AppColors.textSecondary.withValues(alpha: 0.3)),
    );
  }
}
