#ifndef __APP_VERSION_H
#define __APP_VERSION_H

/* ==================== 固件版本单一源头 ====================
 * 升级版本时只需修改 FW_VERSION_STR 这一个字符串,
 * NODE_FW_VERSION 自动跟随 (启动横幅/版本上报/OTA 字符串比较).
 *
 * 版本采用"全程字符串 + 按点分段比较", 支持任意位数/段数
 * (如 v2.321), 不再做数字编码, 避免次版本>255 时溢出错乱(如 2.321 变 3.65).
 * ========================================================= */

#define FW_VERSION_STR       "v2.531"

/* 显示字符串版本: 原样使用 */
#define NODE_FW_VERSION      FW_VERSION_STR

#endif /* __APP_VERSION_H */
