#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET 设备 token 生成器
========================
按 OneNET 官方《Token算法》用 Python 实现,
一条命令生成设备鉴权 token, 免去手动计算的麻烦.

算法核心(官方文档):
  sign = base64( hmac_<method>( base64decode(密钥), 字符串 ) )
  字符串 = et + "\\n" + method + "\\n" + res + "\\n" + version
  最后 res / sign 的 value 要做 URL 编码( / -> %2F, + -> %2B, = -> %3D )

用法:
  交互式(推荐, 按提示输入):
    python token_gen.py

  命令行:
    python token_gen.py "products/<产品ID>/devices/<设备名>" "<设备密钥>"
    python token_gen.py "products/<产品ID>/devices/<设备名>" "<设备密钥>" --method sha1 --days 100

参数说明:
  res      资源名: 产品级 = products/<产品ID>
                  设备级 = products/<产品ID>/devices/<设备名>
  key      设备密钥/产品密钥 (OneNET 控制台 -> 设备详情 -> 鉴权信息 中获取)
  method   签名算法: md5 / sha1 / sha256 (本项目网关/节点用 md5, 默认 md5)
  days     token 有效期天数 (默认 100)

输出示例:
  version=2018-10-31&res=products%2F04kjwU9TC7%2Fdevices%2FPark001&et=1786485232&method=md5&sign=xxxx%3D
"""

import argparse
import base64
import hashlib
import hmac
import sys
import time
import urllib.parse

VERSION = "2018-10-31"
METHODS = ("md5", "sha1", "sha256")


def gen_signature(res, et, access_key, method):
    """计算签名 sign = base64(hmac_<method>(base64decode(key), StringForSignature))

    StringForSignature 按参数名排序: et、method、res、version,
    每个 value 用 '\\n' 连接, 只取 value 不取 key=value 形式.
    """
    text = f"{et}\n{method}\n{res}\n{VERSION}"
    raw_key = base64.b64decode(access_key)  # 密钥先做 base64 解码
    digest = hmac.new(raw_key, text.encode("utf-8"),
                      getattr(hashlib, method)).digest()
    return base64.b64encode(digest).decode()


def make_token(res, access_key, method="md5", days=100):
    """生成完整 token 字符串 (res/sign 的 value 做 URL 编码)"""
    et = int(time.time()) + days * 24 * 3600  # 过期时间(秒)
    sign = gen_signature(res, et, access_key, method)
    token = (
        f"version={VERSION}"
        f"&res={urllib.parse.quote(res, safe='')}"
        f"&et={et}"
        f"&method={method}"
        f"&sign={urllib.parse.quote(sign, safe='')}"
    )
    return token


def run_interactive():
    """交互式输入, 自动生成 token"""
    print("=== OneNET token 生成器 ===")
    print("示例资源名: products/04kjwU9TC7/devices/Park001")
    res = input("资源名(res): ").strip()
    key = input("设备密钥(accessKey): ").strip()
    method = input("签名方法 [md5]: ").strip() or "md5"
    days = input("有效期天数 [100]: ").strip() or "100"
    if method not in METHODS:
        print(f"签名方法只支持: {METHODS}")
        sys.exit(1)
    try:
        days = int(days)
    except ValueError:
        print("天数必须是整数")
        sys.exit(1)
    print("\n生成的 token (填到 config.json):")
    print(make_token(res, key, method, days))


def main():
    parser = argparse.ArgumentParser(description="OneNET 设备 token 生成器")
    parser.add_argument("res", nargs="?", help="资源名, 如 products/04kjwU9TC7/devices/Park001")
    parser.add_argument("key", nargs="?", help="设备密钥(accessKey)")
    parser.add_argument("--method", choices=METHODS, default="md5", help="签名算法 (默认 md5)")
    parser.add_argument("--days", type=int, default=100, help="有效期天数 (默认 100)")
    args = parser.parse_args()

    if not args.res or not args.key:
        run_interactive()
        return

    print(make_token(args.res, args.key, args.method, args.days))


if __name__ == "__main__":
    main()
