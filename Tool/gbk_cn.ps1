# ============================================================
# gbk_cn.ps1 - 把 \xNN 转义序列还原为 GBK 中文
# ============================================================
#
# 用途:
#   STM32 工程的源码是 GBK/GB2312 编码(Keil 默认 ANSI)。
#   如果在 UTF-8 编辑器里直接写中文, 中文字符会以 UTF-8 字节存入,
#   用 GBK 打开后全部变成乱码。
#   安全做法: 在源码里先写 \xNN 转义序列(纯 ASCII, 不污染 GBK 文件),
#   写完后运行本脚本, 把所有转义序列还原成真正的 GBK 中文字符。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File gbk_cn.ps1 <文件1> [文件2 ...]
#   例:  powershell -NoProfile -ExecutionPolicy Bypass -File gbk_cn.ps1 Main\main.c APP\app_tasks.c
#
# 说明:
#   - 只替换"连续两个及以上"的 \xNN 序列, 单个的 \x0D(回车) 这类控制字符不动
#   - 按 GBK 解码后确实含中文才替换, 否则原样保留

$enc = [System.Text.Encoding]::GetEncoding(936)

# 没传文件参数时, 打印用法提示
if ($args.Count -eq 0) {
    Write-Output "用法: gbk_cn.ps1 <文件...>  (把 \xNN 转义还原为 GBK 中文)"
    exit 1
}

foreach ($p in $args) {
    # 跳过不存在的文件
    if (-not (Test-Path $p)) {
        Write-Output ("跳过(文件不存在): " + $p)
        continue
    }

    # 按 GBK 编码读取文件内容
    $content = [System.IO.File]::ReadAllText($p, $enc)

    # 用正则匹配连续 2 个以上的 \xNN 转义序列(如 \xD6\xC7 两个字节拼一个汉字)
    $new = [regex]::Replace($content, '(?:\\x[0-9A-Fa-f]{2}){2,}', {
        param($m)
        $hex = $m.Value -replace '\\x', ''

        # 十六进制串长度必须是偶数, 否则不是完整字节序列, 原样保留
        if (($hex.Length % 2) -ne 0) { return $m.Value }

        # 十六进制字符串转成字节数组
        $len = $hex.Length / 2
        $b = New-Object byte[] $len
        for ($i = 0; $i -lt $len; $i++) {
            $b[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
        }

        # 按 GBK 解码, 只有解码结果确实包含汉字时才替换
        try {
            $decoded = $enc.GetString($b)
            if ($decoded -match '[\u4e00-\u9fff]') { return $decoded }
        } catch { }

        return $m.Value
    })

    # 内容有变化才写回(仍是 GBK 编码, 保持与 Keil 工程一致)
    if ($new -ne $content) {
        [System.IO.File]::WriteAllText($p, $new, $enc)
        Write-Output ("已转换: " + $p)
    } else {
        Write-Output ("无变化: " + $p)
    }
}
