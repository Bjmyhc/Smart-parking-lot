#ifndef __APP_VERSION_H
#define __APP_VERSION_H

/* ==================== 固件版本单一源头 ====================
 * 升级版本时只需修改 FW_VERSION_STR 这一个字符串,
 * APP_VERSION(OTA数字版本)和 NODE_FW_VERSION(显示字符串)自动跟随.
 *
 *   FW_VERSION_STR    : 唯一需要维护的版本, 完整格式 "vX.Y" (X/Y 各一位, 0~9)
 *   NODE_FW_VERSION   : 直接用该字符串 (启动横幅/版本上报)
 *   APP_VERSION       : 编译期从字符串解析成数字 0x0202 (OTA比较/打包文件头)
 * ========================================================= */

#define FW_VERSION_STR       "v2.1"

/* 显示字符串版本: 原样使用 */
#define NODE_FW_VERSION      FW_VERSION_STR

/* 数字版本: 编译期从 "vX.Y" 解析 (主版本=索引1, 次版本=索引3) */
#define FW_VERSION_MAJOR     (FW_VERSION_STR[1] - '0')
#define FW_VERSION_MINOR     (FW_VERSION_STR[3] - '0')
#define APP_VERSION          ((FW_VERSION_MAJOR << 8) | FW_VERSION_MINOR)

#endif /* __APP_VERSION_H */
