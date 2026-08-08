param(
    [string]$curPath,
    [string]$oldPath
)
$ErrorActionPreference = 'Stop'
$gbk = [System.Text.Encoding]::GetEncoding(936)

$cur = $gbk.GetString([System.IO.File]::ReadAllBytes($curPath))
$old = $gbk.GetString([System.IO.File]::ReadAllBytes($oldPath))
$curLines = $cur -split "`r?`n"
$oldLines = $old -split "`r?`n"

function Get-CommentBlocks($lines) {
    $blocks = @()
    $i = 0
    $n = $lines.Count
    while ($i -lt $n) {
        $t = $lines[$i].TrimStart()
        if ($t.StartsWith('/*')) {
            $start = $i
            while ($i -lt $n -and -not $lines[$i].Contains('*/')) { $i++ }
            $end = $i
            $anchor = ''
            $j = $end + 1
            while ($j -lt $n) {
                $tl = $lines[$j].TrimStart()
                if ($tl -eq '' -or $tl.StartsWith('/*') -or $tl.StartsWith('*') -or $tl.StartsWith('//')) { $j++; continue }
                if ($tl -match '\b([A-Za-z_]\w*)\s*\(') { $anchor = $Matches[1]; break }
                else { $j++ }
            }
            $blocks += ,@{ Start = $start; End = $end; Anchor = $anchor }
        }
        $i++
    }
    return $blocks
}

$curBlocks = Get-CommentBlocks $curLines
$oldBlocks = Get-CommentBlocks $oldLines

$oldMap = @{}
foreach ($b in $oldBlocks) {
    if ($b.Anchor -ne '' -and -not $oldMap.ContainsKey($b.Anchor)) { $oldMap[$b.Anchor] = $b }
}

$result = New-Object System.Collections.ArrayList
$i = 0
$n = $curLines.Count
$replaced = @()
while ($i -lt $n) {
    $block = $null
    foreach ($b in $curBlocks) {
        if ($b.Start -eq $i) { $block = $b; break }
    }
    if ($null -ne $block) {
        if ($block.Anchor -ne '' -and $oldMap.ContainsKey($block.Anchor)) {
            $ob = $oldMap[$block.Anchor]
            for ($k = $ob.Start; $k -le $ob.End; $k++) { [void]$result.Add($oldLines[$k]) }
            $replaced += "$($block.Anchor):$($block.Start)-$($block.End)"
        } else {
            for ($k = $block.Start; $k -le $block.End; $k++) { [void]$result.Add($curLines[$k]) }
        }
        $i = $block.End + 1
    } else {
        [void]$result.Add($curLines[$i])
        $i++
    }
}

$out = $result -join "`r`n"
[System.IO.File]::WriteAllBytes($curPath, $gbk.GetBytes($out))
Write-Output "Replaced $($replaced.Count) comment blocks:"
$replaced | ForEach-Object { Write-Output "  $_" }
