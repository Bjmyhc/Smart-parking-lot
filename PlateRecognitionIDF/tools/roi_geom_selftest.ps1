# ROI 几何离线自检 (P5.1: PCA 旋转矩形 + 摆正四角)
#
# 这是 main/main.cpp 里 roi_is_plate_color / 1/4 网格 / roi_morph_h / 连通域 /
# roi_fit_rotated / 候选打分与过滤 的 1:1 复刻 (格点级仿真, 不依赖摄像头和板子)。
# 用 PowerShell 跑, 不需要任何第三方依赖:
#     pwsh -File PlateRecognitionIDF\tools\roi_geom_selftest.ps1
#
# 它验证三件事:
#   1) 旋转矩形拟合出来的倾角 / 长宽比 / 四角位置是否正确 (对着合成的已知车牌比对);
#   2) 倾斜的车牌不会被"轴对齐宽高比"过滤器误杀 (20 / 25 / 33 度那三个 case 是关键);
#   3) P5.2 有效填充率门槛: 又大又稀的"假车牌"必须被拒 (case I 复刻现场日志的 40% 稀块);
#   4) P5.4 贴边拒识: 顶到画面边界的车牌必须被拒 (case J)。
# 当前状态: A~H 全过 + I/J 拒识通过 (倾角误差 <= 0.2 度, 四角偏差 <= 8 px, 旋转比例 3.56~3.84)。
#$ErrorActionPreference='Stop'
$STEP=4; $GW=160; $GH=120; $GRID_N=$GW*$GH
$CELL_NEED=4; $RATIO_LO=2.0; $RATIO_HI=6.5; $RATIO_IDEAL=3.4
$MIN_AREA_RATIO=0.002; $TRIM_X=0.01; $TRIM_Y=0.03; $MAX_SLOTS=32
$MIN_FILL=0.60   # P5.2 有效填充率门槛 (与 main.cpp 的 ROI_MIN_FILL 一致)

function Test-InPlate([double]$px,[double]$py,[double]$cx,[double]$cy,[double]$hw,[double]$hh,[double]$ca,[double]$sa){
  $dx=$px-$cx; $dy=$py-$cy
  $u=$dx*$ca+$dy*$sa; $v=-$dx*$sa+$dy*$ca
  if([Math]::Abs($u) -gt $hw){ return $false }
  if([Math]::Abs($v) -gt $hh){ return $false }
  $pw=$hw*2.0
  $pu=$u+$hw
  for($i=1;$i -le 7;$i++){
    $x0=$pw*$i/8.0
    if($pu -ge $x0 -and $pu -le ($x0+$pw*0.05) -and $v -ge (-$hh*0.30) -and $v -le ($hh*0.30)){ return $false }
    if($pu -ge $x0 -and $pu -le ($x0+$pw*0.06) -and $v -ge ($hh*0.05) -and $v -le ($hh*0.22)){ return $false }
  }
  return $true
}

function Build-Cells([double]$angle,[double]$pw,[double]$ph,[double]$cx,[double]$cy){
  $cell = New-Object 'int[]' $GRID_N
  $ca=[Math]::Cos($angle*[Math]::PI/180.0); $sa=[Math]::Sin($angle*[Math]::PI/180.0)
  $hw=$pw/2.0; $hh=$ph/2.0
  $ex=[Math]::Abs($hw*$ca)+[Math]::Abs($hh*$sa); $ey=[Math]::Abs($hw*$sa)+[Math]::Abs($hh*$ca)
  $gx1=[Math]::Max(0,[int](($cx-$ex)/$STEP)-2); $gx2=[Math]::Min($GW-1,[int](($cx+$ex)/$STEP)+2)
  $gy1=[Math]::Max(0,[int](($cy-$ey)/$STEP)-2); $gy2=[Math]::Min($GH-1,[int](($cy+$ey)/$STEP)+2)
  $off=@(-1.5,-0.5,0.5,1.5)
  for($gy=$gy1;$gy -le $gy2;$gy++){
    for($gx=$gx1;$gx -le $gx2;$gx++){
      $n=0
      foreach($oy in $off){ foreach($ox in $off){
        if(Test-InPlate ($gx*$STEP+2.0+$ox) ($gy*$STEP+2.0+$oy) $cx $cy $hw $hh $ca $sa){ $n++ }
      }}
      $cell[$gy*$GW+$gx]=$n
    }
  }
  return ,$cell
}

function Find-Root($parent,[int]$i){
  $r=$i
  while($parent[$r] -ne $r){ $r=$parent[$r] }
  while($parent[$i] -ne $r){ $t=$parent[$i]; $parent[$i]=$r; $i=$t }
  return $r
}

function Fit-Rotated($cell,$bin,$parent,[int]$x1,[int]$y1,[int]$x2,[int]$y2,[int]$root){
  $f=@{ok=$false; ratio=0.0; fill=0.0; deg=0.0; uw=0.0; vh=0.0; qx=@(0.0,0.0,0.0,0.0); qy=@(0.0,0.0,0.0,0.0)}
  $px=New-Object 'System.Collections.Generic.List[double]'
  $py=New-Object 'System.Collections.Generic.List[double]'
  for($y=$y1;$y -le $y2;$y++){
    for($x=$x1;$x -le $x2;$x++){
      $i=$y*$GW+$x
      if($bin[$i] -eq 0 -or $cell[$i] -lt $CELL_NEED){ continue }
      if((Find-Root $parent $i) -ne $root){ continue }
      $px.Add([double]$x); $py.Add([double]$y)
    }
  }
  $np=$px.Count
  if($np -lt 30){ return $f }
  $mx=0.0;$my=0.0; for($k=0;$k -lt $np;$k++){ $mx+=$px[$k]; $my+=$py[$k] }
  $mx/=$np; $my/=$np
  $cxx=0.0;$cyy=0.0;$cxy=0.0
  for($k=0;$k -lt $np;$k++){ $dx=$px[$k]-$mx; $dy=$py[$k]-$my; $cxx+=$dx*$dx; $cyy+=$dy*$dy; $cxy+=$dx*$dy }
  $cxx/=$np; $cyy/=$np; $cxy/=$np
  $th=0.5*[Math]::Atan2(2.0*$cxy, $cxx-$cyy)
  $ux=[Math]::Cos($th); $uy=[Math]::Sin($th)
  $umin=1e9;$umax=-1e9;$vmin=1e9;$vmax=-1e9
  for($k=0;$k -lt $np;$k++){
    $dx=$px[$k]-$mx; $dy=$py[$k]-$my
    $u=$dx*$ux+$dy*$uy; $v=-$dx*$uy+$dy*$ux
    if($u -lt $umin){$umin=$u}; if($u -gt $umax){$umax=$u}
    if($v -lt $vmin){$vmin=$v}; if($v -gt $vmax){$vmax=$v}
  }
  $uw=$umax-$umin; $vh=$vmax-$vmin
  if($uw -lt 6.0 -or $vh -lt 3.0){ return $f }
  $ratio=$uw/$vh; $fill=$np/(($uw+1.0)*($vh+1.0))
  $f.ratio=$ratio; $f.fill=$fill; $f.deg=$th*180.0/[Math]::PI; $f.uw=$uw; $f.vh=$vh
  if($ratio -lt 1.2 -or $ratio -gt 8.0){ return $f }
  if($fill -lt 0.45){ return $f }
  $um0=$umin+$uw*$TRIM_X; $um1=$umax-$uw*$TRIM_X
  $vm0=$vmin+$vh*$TRIM_Y; $vm1=$vmax-$vh*$TRIM_Y
  if($um1 -le $um0 -or $vm1 -le $vm0){ return $f }
  $uu=@($um0,$um1,$um1,$um0); $vv=@($vm0,$vm0,$vm1,$vm1)
  $cxs=New-Object 'double[]' 4; $cys=New-Object 'double[]' 4
  for($k=0;$k -lt 4;$k++){
    $gx=$mx+$uu[$k]*$ux-$vv[$k]*$uy; $gy=$my+$uu[$k]*$uy+$vv[$k]*$ux
    $cxs[$k]=($gx+0.5)*$STEP; $cys[$k]=($gy+0.5)*$STEP
  }
  $st=0
  for($k=1;$k -lt 4;$k++){ if(($cxs[$k]+$cys[$k]) -lt ($cxs[$st]+$cys[$st])){ $st=$k } }
  $nxt=($st+1)%4
  if($cxs[($st+3)%4] -gt $cxs[$nxt]){ $nxt=($st+3)%4 }
  $ord=@($st,$nxt,(($st+2)%4),(($nxt+2)%4))
  $qx=New-Object 'double[]' 4; $qy=New-Object 'double[]' 4
  for($k=0;$k -lt 4;$k++){ $qx[$k]=$cxs[$ord[$k]]; $qy[$k]=$cys[$ord[$k]] }
  $f.qx=$qx; $f.qy=$qy; $f.ok=$true
  return $f
}

function Roi-Locate($cell){
  $bin = New-Object 'int[]' $GRID_N
  $nc=0
  for($i=0;$i -lt $GRID_N;$i++){ if($cell[$i] -ge $CELL_NEED){ $bin[$i]=1; $nc++ } }
  if($nc -eq 0){ return $null }
  $tmp = New-Object 'int[]' $GRID_N
  foreach($opp in @(0,1)){
    for($y=0;$y -lt $GH;$y++){ for($x=0;$x -lt $GW;$x++){
      $acc=$opp*1
      for($k=-2;$k -le 1;$k++){ $xx=$x+$k; if($xx -lt 0 -or $xx -ge $GW){ continue }
        $s=$bin[$y*$GW+$xx]; if($opp -eq 1){ $acc=$acc -band $s } else { $acc=$acc -bor $s } }
      $tmp[$y*$GW+$x]=$acc } }
    $bin,$tmp=$tmp,$bin
    for($y=0;$y -lt $GH;$y++){ for($x=0;$x -lt $GW;$x++){
      $acc=1-$opp
      for($k=-2;$k -le 1;$k++){ $xx=$x+$k; if($xx -lt 0 -or $xx -ge $GW){ continue }
        $s=$bin[$y*$GW+$xx]; if($opp -eq 0){ $acc=$acc -band $s } else { $acc=$acc -bor $s } }
      $tmp[$y*$GW+$x]=$acc } }
    $bin,$tmp=$tmp,$bin
  }
  $parent = New-Object 'int[]' $GRID_N
  for($i=0;$i -lt $GRID_N;$i++){ if($bin[$i] -eq 1){ $parent[$i]=$i } else { $parent[$i]=-1 } }
  for($y=0;$y -lt $GH;$y++){ for($x=0;$x -lt $GW;$x++){
    $i=$y*$GW+$x; if($bin[$i] -eq 0){ continue }
    if($x -gt 0 -and $bin[$i-1] -eq 1){ $a=Find-Root $parent $i; $b=Find-Root $parent ($i-1); if($a -ne $b){ $parent[$a]=$b } }
    if($y -gt 0 -and $bin[$i-$GW] -eq 1){ $a=Find-Root $parent $i; $b=Find-Root $parent ($i-$GW); if($a -ne $b){ $parent[$a]=$b } } } }
  $slRoot=@(); $slX1=@(); $slY1=@(); $slX2=@(); $slY2=@(); $slArea=@()
  for($y=0;$y -lt $GH;$y++){ for($x=0;$x -lt $GW;$x++){
    $i=$y*$GW+$x; if($bin[$i] -eq 0){ continue }
    $root=Find-Root $parent $i; $s=-1
    for($k=0;$k -lt $slRoot.Count;$k++){ if($slRoot[$k] -eq $root){ $s=$k; break } }
    if($s -lt 0){ if($slRoot.Count -ge $MAX_SLOTS){ continue }
      $slRoot+=$root; $slX1+=$x; $slY1+=$y; $slX2+=$x; $slY2+=$y; $slArea+=0; $s=$slRoot.Count-1 }
    if($x -lt $slX1[$s]){ $slX1[$s]=$x }; if($x -gt $slX2[$s]){ $slX2[$s]=$x }
    if($y -lt $slY1[$s]){ $slY1[$s]=$y }; if($y -gt $slY2[$s]){ $slY2[$s]=$y }
    $slArea[$s]=$slArea[$s]+1 } }
  $minCells=[int]($GRID_N*$MIN_AREA_RATIO)
  $n=$slRoot.Count
  $cOk=New-Object 'bool[]' $n; $cRatio=New-Object 'double[]' $n; $cAxis=New-Object 'double[]' $n
  $cDens=New-Object 'double[]' $n; $cDenseff=New-Object 'double[]' $n; $cScore=New-Object 'double[]' $n
  $cOkr=New-Object 'bool[]' $n; $densRej=0
  $cFit=New-Object 'object[]' $n
  for($k=0;$k -lt $n;$k++){
    $cFit[$k]=$null
    if($slArea[$k] -lt $minCells){ continue }
    $bw=$slX2[$k]-$slX1[$k]+1.0; $bh=$slY2[$k]-$slY1[$k]+1.0
    $cAxis[$k]=$bw/$bh
    $f=Fit-Rotated $cell $bin $parent $slX1[$k] $slY1[$k] $slX2[$k] $slY2[$k] $slRoot[$k]
    $cFit[$k]=$f
    $ratio = if($f.ok){ $f.ratio } else { $cAxis[$k] }
    $cRatio[$k]=$ratio
    if($ratio -lt $RATIO_LO -or $ratio -gt $RATIO_HI){ continue }
    $cOkr[$k]=$true
    $cDens[$k]=$slArea[$k]/($bw*$bh)
    # 有效填充率: 摆正得了就用旋转矩形填充率(与倾角无关), 否则退回轴对齐外接框密度
    $cDenseff[$k] = if($f.ok){ $f.fill } else { $cDens[$k] }
    $cScore[$k]=$slArea[$k]/(1.0+[Math]::Abs($ratio-$RATIO_IDEAL))
    if($cDenseff[$k] -lt $MIN_FILL){ $densRej++; continue }
    $cOk[$k]=$true
  }
  $best=-1
  for($k=0;$k -lt $n;$k++){ if(-not $cOk[$k]){ continue }; if($best -lt 0 -or $cScore[$k] -gt $cScore[$best]){ $best=$k } }
  $script:lastCand=$n; $script:lastRatioOk=($cOkr | Where-Object {$_} | Measure-Object).Count; $script:lastDensRej=$densRej
  if($best -lt 0){ return $null }
  # P5.4: 连通域顶到画面边界 => 车牌被切掉, 一律拒绝 (main.cpp 的 4.5 步)
  $clipL=($slX1[$best] -eq 0); $clipR=(($slX2[$best]+1)*$STEP -ge $GW*$STEP)
  $clipT=($slY1[$best] -eq 0); $clipB=(($slY2[$best]+1)*$STEP -ge $GH*$STEP)
  $script:lastEdge=""
  if($clipL){$script:lastEdge+="左"}; if($clipR){$script:lastEdge+="右"}
  if($clipT){$script:lastEdge+="上"}; if($clipB){$script:lastEdge+="下"}
  if($clipL -or $clipR -or $clipT -or $clipB){ return $null }
  $r=@{}
  $r.x1=$slX1[$best]*$STEP; $r.y1=$slY1[$best]*$STEP
  $r.x2=($slX2[$best]+1)*$STEP; $r.y2=($slY2[$best]+1)*$STEP
  $r.axis_ratio=$cAxis[$best]
  $r.ratio=$cRatio[$best]
  $r.dens=$cDens[$best]; $r.denseff=$cDenseff[$best]; $r.score=$cScore[$best]
  $r.area_pct=100.0*($slX2[$best]-$slX1[$best]+1.0)*($slY2[$best]-$slY1[$best]+1.0)/$GRID_N
  $r.rot_ok=$cFit[$best].ok
  $r.rot_ratio=$cFit[$best].ratio; $r.rot_fill=$cFit[$best].fill; $r.rot_deg=$cFit[$best].deg
  $r.qx=$cFit[$best].qx; $r.qy=$cFit[$best].qy
  if($r.rot_ok){ $r.area_pct=100.0*$cFit[$best].uw*$cFit[$best].vh/$GRID_N }
  return $r
}

function Expected-Quad([double]$angle,[double]$pw,[double]$ph,[double]$cx,[double]$cy){
  $ca=[Math]::Cos($angle*[Math]::PI/180.0); $sa=[Math]::Sin($angle*[Math]::PI/180.0)
  $hw=$pw/2.0-$pw*$TRIM_X; $hh=$ph/2.0-$ph*$TRIM_Y
  $qx=New-Object 'double[]' 4; $qy=New-Object 'double[]' 4
  $i=0
  foreach($d in @(@(-1,-1),@(1,-1),@(1,1),@(-1,1))){
    $lx=$d[0]*$hw; $ly=$d[1]*$hh
    $qx[$i]=$cx+$lx*$ca-$ly*$sa; $qy[$i]=$cy+$lx*$sa+$ly*$ca; $i++
  }
  return @($qx,$qy)
}

# 造一个"又大又稀"的假车牌: 复刻现场日志第 1 次会话 -- 外接框 106x42 格(=> 比例 2.52),
# 上半 8 行 + 下半 8 行整行是"车牌色", 中间 26 行只有最左 3 格相连 (整体是 U/C 形, 中间大片空洞)。
# 期望: 形状像车牌但有效填充只有 ~40% => 必须被拒 (宁可不猜也不出字)。
function Build-SparseBlob(){
  $cell = New-Object 'int[]' $GRID_N
  $x1=0; $x2=105; $y1=78; $y2=119
  for($gy=$y1;$gy -le $y2;$gy++){
    for($gx=$x1;$gx -le $x2;$gx++){
      $solid = ($gy -le ($y1+7)) -or ($gy -ge ($y2-7)) -or ($gx -le ($x1+2))
      if($solid){ $cell[$gy*$GW+$gx]=16 }
    }
  }
  return ,$cell
}

function Run-RejectCase([string]$name){
  $cell=Build-SparseBlob
  $b=Roi-Locate $cell
  Write-Host ("-"*72)
  Write-Host ("{0}" -f $name)
  Write-Host ("  候选总数 {0}, 过宽高比 {1}, 被有效填充门槛淘汰 {2}" -f $script:lastCand,$script:lastRatioOk,$script:lastDensRej)
  if($b -ne $null){
    Write-Host ("  !! 竟然接受了: x[{0},{1}) y[{2},{3}) 比例 {4:N2} 有效填充 {5:N0}%" -f $b.x1,$b.x2,$b.y1,$b.y2,$b.ratio,($b.denseff*100))
    return $false
  }
  if($script:lastDensRej -lt 1){ throw "$name : 返回 null 了, 但不是被填充率门槛拒的" }
  Write-Host "  OK: 已拒识 (没猜出一种车牌号)"
  return $true
}

# 造一个"正好顶到画面右边缘"的正牌: 几何完全合格, 只是有一截在画面外。
# 期望: 被拒识 (main.cpp P5.4 的贴边拒识), 因为裁出来的图必然缺字。
function Run-EdgeCase([string]$name){
  $cell=Build-Cells 0.0 300.0 80.0 (640.0-150.0) 260.0
  $b=Roi-Locate $cell
  Write-Host ("-"*72)
  Write-Host ("{0}" -f $name)
  if($b -ne $null){
    Write-Host ("  !! 竟然接受了: x[{0},{1}) 比例 {2:N2}" -f $b.x1,$b.x2,$b.ratio)
    return $false
  }
  if($script:lastEdge -ne "右"){ throw ("$name : 返回 null 了, 但不是因为贴边 (lastEdge='" + $script:lastEdge + "')") }
  Write-Host ("  OK: 已拒识 (贴到画面{0}边缘)" -f $script:lastEdge)
  return $true
}

function Run-Case([string]$name,[double]$angle,[double]$pw,[double]$ph,[double]$cx,[double]$cy){
  $cell=Build-Cells $angle $pw $ph $cx $cy
  $b=Roi-Locate $cell
  Write-Host ("-"*72)
  if($b -eq $null){ Write-Host "$name : NO CANDIDATE (plate rejected)"; return $false }
  Write-Host ("{0}: plate {1}x{2} @({3},{4}) angle {5}" -f $name,$pw,$ph,$cx,$cy,$angle)
  Write-Host ("  axis box x[{0},{1}) y[{2},{3}) 轴对齐比例 {4:N2} 密度 {5:N0}% 得分 {6:N0} 占屏 {7:N1}%" -f $b.x1,$b.x2,$b.y1,$b.y2,$b.axis_ratio,($b.dens*100),$b.score,$b.area_pct)
  Write-Host ("  fit: ok={0} 旋转比例 {1:N2} 倾角 {2:N1} 填充 {3:N0}%" -f $b.rot_ok,$b.rot_ratio,$b.rot_deg,($b.rot_fill*100))
  Write-Host ("  P5.2 有效填充 {0:N0}% (门槛 {1:N0}%, 本帧淘汰 {2} 个)" -f ($b.denseff*100),($MIN_FILL*100),$script:lastDensRej)
  if(-not $b.rot_ok){ throw "$name : rot_ok=false" }
  if($b.denseff -lt $MIN_FILL){ throw ("$name : 有效填充 {0:N0}% 低于门槛, 真车牌不该被淘汰" -f ($b.denseff*100)) }
  $exp=Expected-Quad $angle $pw $ph $cx $cy
  $maxe=0.0
  $s=""
  for($k=0;$k -lt 4;$k++){
    $dx=$b.qx[$k]-$exp[0][$k]; $dy=$b.qy[$k]-$exp[1][$k]
    $e=[Math]::Sqrt($dx*$dx+$dy*$dy); if($e -gt $maxe){ $maxe=$e }
    $s += ("({0:N0},{1:N0}) " -f $b.qx[$k],$b.qy[$k])
  }
  Write-Host ("  quad TL/TR/BR/BL: {0}" -f $s)
  Write-Host ("  四角与真值最大偏差 {0:N1} px" -f $maxe)
  $tlx=$b.qx[0];$tly=$b.qy[0];$trx=$b.qx[1];$try=$b.qy[1]
  $blx=$b.qx[3];$bly=$b.qy[3];$brx=$b.qx[2];$bry=$b.qy[2]
  $okOrder = ([Math]::Abs($trx-$tlx) -gt [Math]::Abs($try-$tly)) -and ([Math]::Abs($bly-$tly) -gt [Math]::Abs($blx-$tlx)) -and ($blx -lt $brx)
  Write-Host ("  顺序自检(TL->TR 横向, TL->BL 纵向, BL 在 BR 左侧): {0}" -f $okOrder)
  if(-not $okOrder){ throw "$name : corner order wrong" }
  if($maxe -gt 16.0){ throw ("$name : corner err {0:N1} px" -f $maxe) }
  if([Math]::Abs($b.rot_deg - $angle) -gt 2.0){ throw ("$name : deg {0:N1} vs {1}" -f $b.rot_deg,$angle) }
  $expRatio=($pw-2*$pw*$TRIM_X)/($ph-2*$ph*$TRIM_Y)
  if([Math]::Abs($b.rot_ratio-$expRatio) -gt 0.30*$expRatio){ throw ("$name : ratio {0:N2} vs {1:N2}" -f $b.rot_ratio,$expRatio) }
  return $true
}

Write-Output "P5.4 ROI geometry self-test v4 (cell-level, 1:1 port of main.cpp; 160x120 grid)"
$fail=0; $pass=0
$cases=@(
  [pscustomobject]@{n='A upright';    a=0.0;   w=300.0; h=80.0;  x=320.0; y=260.0},
  [pscustomobject]@{n='B tilt +8';    a=8.0;   w=300.0; h=80.0;  x=320.0; y=260.0},
  [pscustomobject]@{n='C tilt -10';   a=-10.0; w=260.0; h=70.0;  x=300.0; y=240.0},
  [pscustomobject]@{n='D tilt +15 sm';a=15.0;  w=180.0; h=48.0;  x=250.0; y=300.0},
  [pscustomobject]@{n='E tilt -6 big';a=-6.0;  w=420.0; h=108.0; x=320.0; y=240.0},
  [pscustomobject]@{n='F tilt +20';   a=20.0;  w=240.0; h=64.0;  x=320.0; y=240.0},
  [pscustomobject]@{n='G tilt -25';   a=-25.0; w=200.0; h=56.0;  x=300.0; y=260.0},
  [pscustomobject]@{n='H tilt +33';   a=33.0;  w=220.0; h=60.0;  x=320.0; y=250.0}
)
foreach($c in $cases){
  try { if(Run-Case $c.n $c.a $c.w $c.h $c.x $c.y){ $pass++ } else { $fail++ } }
  catch { Write-Host ("  !! FAIL: " + $_.Exception.Message); $fail++ }
}
try { if(Run-RejectCase "I sparse blob (现场日志#1: 106x42 格, 密度 40%)"){ $pass++ } else { $fail++ } }
catch { Write-Host ("  !! FAIL: " + $_.Exception.Message); $fail++ }
try { if(Run-EdgeCase "J plate at right frame edge (现场日志: 贴边 25 轮里 24 轮乱码)"){ $pass++ } else { $fail++ } }
catch { Write-Host ("  !! FAIL: " + $_.Exception.Message); $fail++ }
Write-Output ("="*72)
Write-Output ("RESULT: pass={0} fail={1}" -f $pass,$fail)