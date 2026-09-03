import 'package:flutter/material.dart';
import '../../core/theme/app_dims.dart';
import 'page_title.dart';

/// 4 个主页面(首页/车位/告警/我的)统一的顶部栏.
/// 标题样式走 [PageTitle]; 标题的顶部+左右边距在此一处维护, 改一处 4 页同步.
class PageHeader extends StatelessWidget {
  final String title;
  final VoidCallback? onTitleTap;

  /// 标题右侧操作区 (状态徽章 / 切换按钮 / 搜索图标等), 按页面需要传入
  final List<Widget>? actions;

  /// 是否自带左右页边距.
  /// 首页内容在 ReorderableListView 内, 列表已提供左右边距, 传 false 避免重复叠加.
  final bool addHorizontalPadding;

  const PageHeader({
    super.key,
    required this.title,
    this.onTitleTap,
    this.actions,
    this.addHorizontalPadding = true,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        top: MediaQuery.of(context).padding.top + 16, // ← 标题距状态栏间距, 统一调
        left: addHorizontalPadding ? AppDims.paddingPage : 0,
        right: addHorizontalPadding ? AppDims.paddingPage : 0,
      ),
      child: Row(
        children: [
          Expanded(child: PageTitle(title, onTap: onTitleTap)),
          ...?actions,
        ],
      ),
    );
  }
}
