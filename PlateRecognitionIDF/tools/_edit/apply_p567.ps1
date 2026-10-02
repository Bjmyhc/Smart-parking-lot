$ErrorActionPreference = "Stop"
$root = "G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF"
$p    = "$root\main\main.cpp"
$d    = "$root\tools\_edit"

Copy-Item -LiteralPath $p -Destination "$root\main\main.cpp.bak_p566" -Force
$txt = [IO.File]::ReadAllText($p)
if ($txt.Contains("`r")) { throw "main.cpp 里有 CR, 先停下" }

function Snip($name) {
    $s = [IO.File]::ReadAllText("$d\$name")
    if ($s.Contains("`r")) { throw "$name 里有 CR" }
    return ,([string[]]($s.TrimEnd("`n") -split "`n"))
}

$L = New-Object System.Collections.Generic.List[string]
$L.AddRange([string[]]($txt -split "`n"))

# ---------- 断言: 三个锚点区间的边界行确实是我们以为的那些 ----------
function AssertLine($n, $expect) {
    $cur = $L[$n - 1].Trim()
    if ($cur -ne $expect) { throw "第 $n 行不是预期内容`n  预期: $expect`n  实际: $cur" }
}
AssertLine 1074  "}"
AssertLine 1075  ""
AssertLine 2511  "int prov_id = -1, prov_id2 = -1, prov_ms = 0;"
AssertLine 2533  "}"
AssertLine 2546  'char provbuf[128] = "";'
AssertLine 2558  "}"

$s1 = Snip "p567_s1.txt"
$s2 = Snip "p567_s2.txt"
$s3 = Snip "p567_s3.txt"

function Repl($a, $b, $new) {           # 替换第 a..b 行 (1-based 闭区间)
    $L.RemoveRange($a - 1, $b - $a + 1)
    $L.InsertRange($a - 1, $new)
}
function Ins($after, $new) {          # 在第 after 行之后插入
    $L.InsertRange($after, $new)
}

Repl 2546 2558 $s3
Repl 2511 2533 $s2
Ins 1074 $s1

$txt = ($L -join "`n")

# ---------- 定点替换 (每处都必须命中且只命中一次) ----------
function Rep($old, $new, $tag) {
    $n = ([regex]::Matches($script:txt, [regex]::Escape($old))).Count
    if ($n -ne 1) { throw "[$tag] 锚点命中 $n 次 (应为 1)" }
    $script:txt = $script:txt.Replace($old, $new)
}

$r1 = [IO.File]::ReadAllText("$d\p567_r1.txt")
$r6 = [IO.File]::ReadAllText("$d\p567_r6.txt")

Rep "static const float PROV_CONF_MIN = 0.70f;`n" ("static const float PROV_CONF_MIN = 0.70f;`n" + $r1) "R1 常量"

Rep "static uint8_t *g_tx_jpg = nullptr;              // JPEG 输出缓冲, PSRAM`n" `
    ("static uint8_t *g_tx_jpg = nullptr;              // JPEG 输出缓冲, PSRAM`n" +
     "static uint8_t *g_prov_rgb = nullptr;            // P5.67: 省字 patch 的原色 RGB (PROV_W*PROV_H*3), 详细模式/低置信时发回串口`n") "R2 缓冲声明"

Rep "                                 const float *qx, const float *qy,`n                                 int8_t *dst, const int8_t *lut) {" `
    "                                 const float *qx, const float *qy, float u_base,`n                                 int8_t *dst, const int8_t *lut) {" "R3 取样签名"

Rep "    for (int y = 0; y < PROV_H; y++) {`n        const float v0 = (float)y * dv;`n        for (int x = 0; x < PROV_W; x++) {`n            const float u0 = (float)x * du;" `
    "    for (int y = 0; y < PROV_H; y++) {`n        const float v0 = (float)y * dv;`n        for (int x = 0; x < PROV_W; x++) {`n            // P5.67: u_base = 这扇窗口的左边界 (由蓝面锚点/候选决定), 老做法就是 0`n            const float u0 = u_base + (float)x * du;" "R4 窗口起点"

Rep "            // 通道序 R,G,B —— 与训练时的 im[:,:,::-1] (BGR->RGB) 一致; 三个通道的 LUT 相同`n            int8_t *o = dst + ((size_t)y * PROV_W + x) * 3;" `
    ("            // P5.67: 顺手把这块 patch 的原色抄一份 —— 详细模式(或复核没过门槛时)会把它发回串口`n" +
     "            if (g_prov_rgb != nullptr) {`n" +
     "                uint8_t *pd = g_prov_rgb + ((size_t)y * PROV_W + x) * 3;`n" +
     "                pd[0] = (uint8_t)rr;`n" +
     "                pd[1] = (uint8_t)gg;`n" +
     "                pd[2] = (uint8_t)bb;`n" +
     "            }`n" +
     "            // 通道序 R,G,B —— 与训练时的 im[:,:,::-1] (BGR->RGB) 一致; 三个通道的 LUT 相同`n            int8_t *o = dst + ((size_t)y * PROV_W + x) * 3;") "R5 RGB 抄写"

Rep "            g_prov_lut = (int8_t *)malloc(3 * 256);`n" `
    ("            g_prov_lut = (int8_t *)malloc(3 * 256);`n" +
     "            // P5.67: 发图用的 patch 原色缓冲 (PSRAM); 分不到只是不发那张诊断图, 不影响复核本身`n" +
     "            g_prov_rgb = (uint8_t *)heap_caps_aligned_calloc(16, 1, (size_t)PROV_W * PROV_H * 3,`n" +
     "                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);`n") "R7 缓冲分配"

Rep "/**`n * P5.66: 从车牌四边形里抠出「省字那一格」-> 32x64 的省字模型输入 (RGB 序, 用省字 LUT 量化)。" `
    ($r6 + "/**`n * P5.66: 从车牌四边形里抠出「省字那一格」-> 32x64 的省字模型输入 (RGB 序, 用省字 LUT 量化)。") "R6 锚点函数"

$txt = $txt.Replace("`r", "")
[IO.File]::WriteAllText($p, $txt, (New-Object Text.UTF8Encoding($false)))
"OK: 新文件 $($txt.Length) 字节 / $((($txt -split "`n")).Count) 行"