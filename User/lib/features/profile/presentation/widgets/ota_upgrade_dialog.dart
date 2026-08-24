import 'package:flutter/material.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';

/// OTA 升级提示弹窗 (全局, 由 MainShell 检测到待升级任务时触发).
///
/// 状态流转:
/// - 未确认: 显示 当前版本→目标版本, [忽略][立即升级]
/// - 立即升级: 下发 OtaAllow=true 确认, 弹窗转为进度展示(轮询任务状态)
/// - 完成/失败: 显示结果, [完成] 关闭; 忽略后可从"固件升级"页手动升级
class OtaUpgradeDialog extends StatelessWidget {
  const OtaUpgradeDialog({super.key, required this.provider});

  final ParkingProvider provider;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: provider,
      builder: (context, _) {
        final confirming = provider.otaConfirming;
        final status = provider.otaStatus;
        final done = status != null && status >= 4;
        final failed = status == 5 || status == 6;

        return AlertDialog(
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
          ),
          title: Row(
            children: [
              const Icon(Icons.system_update_alt, color: AppColors.primary),
              const SizedBox(width: 10),
              Text(
                confirming
                    ? '固件升级中'
                    : done
                        ? (failed ? '固件升级失败' : '固件升级完成')
                        : '检测到新固件',
              ),
            ],
          ),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              if (done && !failed)
                // 升级成功: 已是最新版本, 不再显示旧版本/目标版本
                const Row(
                  children: [
                    Icon(Icons.check_circle, color: AppColors.success, size: 18),
                    SizedBox(width: 6),
                    Text(
                      '当前已是最新版本',
                      style: TextStyle(
                        fontSize: 14,
                        fontWeight: FontWeight.w600,
                        color: AppColors.textPrimary,
                      ),
                    ),
                  ],
                )
              else ...[
                _row('当前版本', provider.otaCurrentVersion ?? '—'),
                const SizedBox(height: 8),
                _row('目标版本', provider.otaTarget ?? '—'),
              ],
              const SizedBox(height: 16),
              if (provider.otaShowProgress)
                Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Text(
                          provider.otaStatusText,
                          style: TextStyle(
                            fontSize: 14,
                            fontWeight: FontWeight.w600,
                            color: failed
                                ? AppColors.danger
                                : AppColors.primary,
                          ),
                        ),
                        const Spacer(),
                        Text(
                          '${provider.otaStep}%',
                          style: const TextStyle(
                            fontSize: 14,
                            fontWeight: FontWeight.w700,
                            color: AppColors.textPrimary,
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 8),
                    ClipRRect(
                      borderRadius: BorderRadius.circular(4),
                      child: LinearProgressIndicator(
                        value: provider.otaStep / 100,
                        minHeight: 8,
                        backgroundColor:
                            AppColors.primary.withValues(alpha: 0.15),
                        color: failed ? AppColors.danger : AppColors.primary,
                      ),
                    ),
                  ],
                )
              else if (done && failed)
                const Text(
                  '升级失败，请点击完成关闭',
                  style: TextStyle(
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                    color: AppColors.danger,
                  ),
                )
              else if (done)
                const SizedBox.shrink() // 成功: 标题"固件升级完成"已说明, 无需重复文案
              else
                const Text(
                  '有新版本可用，是否立即升级？',
                  style: TextStyle(fontSize: 14),
                ),
            ],
          ),
          actions: [
            if (!confirming && !done)
              TextButton(
                onPressed: () {
                  provider.dismissOtaPrompt(ignore: true);
                  Navigator.of(context).pop();
                },
                child: const Text('忽略'),
              ),
            if (!confirming && !done)
              FilledButton(
                style: FilledButton.styleFrom(
                  backgroundColor: AppColors.primary,
                ),
                onPressed: () async {
                  final ok = await provider.confirmOtaUpgrade();
                  if (!ok && context.mounted) {
                    ScaffoldMessenger.of(context).showSnackBar(
                      const SnackBar(content: Text('确认指令下发失败，请重试')),
                    );
                  }
                },
                child: const Text('立即升级'),
              ),
            if (done)
              TextButton(
                onPressed: () => Navigator.of(context).pop(),
                child: const Text('完成'),
              ),
          ],
        );
      },
    );
  }

  Widget _row(String label, String value) {
    return Row(
      children: [
        Text(
          label,
          style: const TextStyle(fontSize: 13, color: AppColors.textSecondary),
        ),
        const Spacer(),
        Text(
          value,
          style: const TextStyle(
            fontSize: 14,
            fontWeight: FontWeight.w600,
            color: AppColors.textPrimary,
          ),
        ),
      ],
    );
  }
}
