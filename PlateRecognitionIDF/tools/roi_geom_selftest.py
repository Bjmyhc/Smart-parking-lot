# 已被同目录下的 roi_geom_selftest.ps1 取代 (那个版本才真正跑过并 8/8 通过)。
#
# 原因: 本机沙箱里起不来 python.exe, 所以这份 python 版从未被执行过, 不敢留一个
# 没验证过的脚本当"证据"。请用:
#     pwsh -File PlateRecognitionIDF\tools\roi_geom_selftest.ps1