import 'package:flutter/material.dart';
import '../../../../core/providers/parking_provider.dart';
import '../../../../core/theme/app_colors.dart';

/// OTA 升级提示弹窗 (全局, 由 MainShell 检测到待升级任务时触发).
/// ⭐ 底部上弹式浮空卡片 (ModalBottomSheet): 左右下方留空隙 + 全圆角 + 阴影,
/// 比居中弹窗更符合移动端操作直觉, 拇指可达, 视觉更轻.
///
/// 状态流转:
/// - 未确认: 显示 当前版本→目标版本 + 升级要点, [忽略][立即升级]
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
        // ⭐ 未确认阶段才显示升级要点 (升级中/完成不需要)
        final showUpdateInfo = !confirming && !done;

        // ⭐ 外层 Padding: 左右下方留 12 空隙, 卡片浮空不贴边
        return Padding(
          padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
          child: Container(
            // ⭐ 全圆角 (四角都圆) + 阴影, 配合外层空隙形成浮空卡片
            decoration: BoxDecoration(
              color: Colors.white,
              borderRadius: BorderRadius.circular(24),
              boxShadow: [
                BoxShadow(
                  color: Colors.black.withValues(alpha: 0.08),
                  blurRadius: 24,
                  offset: const Offset(0, -4),
                ),
              ],
            ),
            padding: EdgeInsets.fromLTRB(
              20,
              12,
              20,
              16 + MediaQuery.of(context).padding.bottom,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                // 顶部拖拽条
                Center(
                  child: Container(
                    width: 36,
                    height: 4,
                    decoration: BoxDecoration(
                      color: AppColors.textSecondary.withValues(alpha: 0.3),
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                ),
                const SizedBox(height: 16),
                // 标题
                Row(
                  children: [
                    const Icon(Icons.system_update_alt,
                        color: AppColors.primary),
                    const SizedBox(width: 10),
                    Text(
                      confirming
                          ? '固件升级中'
                          : done
                              ? (failed ? '固件升级失败' : '固件升级完成')
                              : '检测到新固件',
                      style: const TextStyle(
                        fontSize: 18,
                        fontWeight: FontWeight.w600,
                        color: AppColors.textPrimary,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 16),
                // 版本对比区
                if (done && !failed)
                  const Row(
                    children: [
                      Icon(Icons.check_circle,
                          color: AppColors.success, size: 18),
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
                // ⭐ 未确认阶段: 升级要点列表, 增加信息量与说服力, 拉开高度
                if (showUpdateInfo) ...[
                  const SizedBox(height: 16),
                  Container(
                    height: 0.5,
                    color: AppColors.textSecondary.withValues(alpha: 0.15),
                  ),
                  const SizedBox(height: 14),
                  _bulletPoint('修复已知问题'),
                  _bulletPoint('提升系统稳定性'),
                  _bulletPoint('优化运行性能'),
                ],
                const SizedBox(height: 16),
                // 进度/状态区
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
                  const SizedBox.shrink()
                else
                  const Text(
                    '有新版本可用，是否立即升级？',
                    style: TextStyle(
                      fontSize: 14,
                      color: AppColors.textSecondary,
                    ),
                  ),
                const SizedBox(height: 20),
                // 按钮区
                if (!confirming && !done)
                  Row(
                    children: [
                      Expanded(
                        child: OutlinedButton(
                          style: OutlinedButton.styleFrom(
                            padding: const EdgeInsets.symmetric(vertical: 12),
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(12),
                            ),
                            side: BorderSide(
                              color: AppColors.textSecondary
                                  .withValues(alpha: 0.3),
                            ),
                          ),
                          onPressed: () {
                            provider.dismissOtaPrompt(ignore: true);
                            Navigator.of(context).pop();
                          },
                          child: const Text('忽略'),
                        ),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: FilledButton(
                          style: FilledButton.styleFrom(
                            backgroundColor: AppColors.primary,
                            padding: const EdgeInsets.symmetric(vertical: 12),
                            shape: RoundedRectangleBorder(
                              borderRadius: BorderRadius.circular(12),
                            ),
                          ),
                          onPressed: () async {
                            final ok = await provider.confirmOtaUpgrade();
                            if (!ok && context.mounted) {
                              ScaffoldMessenger.of(context).showSnackBar(
                                const SnackBar(
                                    content: Text('确认指令下发失败，请重试')),
                              );
                            }
                          },
                          child: const Text('立即升级'),
                        ),
                      ),
                    ],
                  )
                else if (done)
                  SizedBox(
                    width: double.infinity,
                    child: FilledButton(
                      style: FilledButton.styleFrom(
                        backgroundColor: AppColors.primary,
                        padding: const EdgeInsets.symmetric(vertical: 12),
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(12),
                        ),
                      ),
                      onPressed: () => Navigator.of(context).pop(),
                      child: const Text('完成'),
                    ),
                  ),
              ],
            ),
          ),
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

  Widget _bulletPoint(String text) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          Icon(Icons.check_circle,
              size: 16, color: AppColors.primary.withValues(alpha: 0.7)),
          const SizedBox(width: 8),
          Text(text,
              style: const TextStyle(
                  fontSize: 13, color: AppColors.textSecondary)),
        ],
      ),
    );
  }
}
