# dune_bench.ps1 -- set-wide colour-accuracy comparison of matched renderer grabs.
#
# For every shot folder containing one madVR grab and one unplugged grab of the SAME
# frame, measure the content area (letterbox excluded by a luma floor) and report,
# per renderer and as a delta:
#   mean luma, mean saturation, clipped-pixel fraction, mean B-R (tint axis),
#   and the same luma/saturation split into three bands (shadow / mid / highlight)
#   so a difference can be LOCALISED to a tonal region instead of averaged away.
#
# Bands are in delivered 8-bit code, matching the project's own shadow definition
# (ml_features.cpp: (4,48] is the toe band).
param([string]$Root = 'C:\Users\satee\Pictures\Benchmarks\Dune')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Get-Pct($hist, [int]$n, [double]$q) {
    # luma-code value at quantile $q, read off a 256-bin histogram
    $t = [int][math]::Ceiling($q * $n); $c = 0
    for ($i = 0; $i -lt 256; $i++) { $c += $hist[$i]; if ($c -ge $t) { return [double]$i } }
    return 255.0
}

function Measure-Image([string]$path) {
    $im = New-Object System.Drawing.Bitmap($path)
    $w = $im.Width; $h = $im.Height
    $r = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
    $d = $im.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                      [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $buf = New-Object byte[] ($d.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length)
    $im.UnlockBits($d); $im.Dispose()
    $S = $d.Stride

    $n=0; $sumL=0.0; $sumSat=0.0; $clip=0; $sr=0.0; $sg=0.0; $sb=0.0
    $bN = @(0,0,0); $bL = @(0.0,0.0,0.0); $bSat = @(0.0,0.0,0.0)
    # tail metrics: luma histogram (for percentiles) and PER-CHANNEL saturation,
    # because a per-channel curve clips the busiest channel long before all three
    # reach white -- an all-channel test reads 0.000 and sees nothing.
    $hist = New-Object int[] 256; $cR=0; $cG=0; $cB=0; $lMax=0.0
    for ($y = 0; $y -lt $h; $y += 3) {
        $row = $y * $S
        for ($x = 0; $x -lt $w; $x += 3) {
            $o = $row + $x * 3
            $B = [double]$buf[$o]; $G = [double]$buf[$o+1]; $R = [double]$buf[$o+2]
            $L = 0.2126*$R + 0.7152*$G + 0.0722*$B
            if ($L -lt 6.0) { continue }              # letterbox / true black
            $mx = [math]::Max($R, [math]::Max($G, $B))
            $mn = [math]::Min($R, [math]::Min($G, $B))
            $sat = 0.0; if ($mx -gt 0) { $sat = ($mx - $mn) / $mx }
            if ($mn -ge 250.0) { $clip++ }
            $bi = if ($L -le 48) { 0 } elseif ($L -le 160) { 1 } else { 2 }
            $bN[$bi]++; $bL[$bi] += $L; $bSat[$bi] += $sat
            $hist[[int][math]::Min(255.0, [math]::Floor($L))]++
            if ($L -gt $lMax) { $lMax = $L }
            if ($R -ge 254.0) { $cR++ }
            if ($G -ge 254.0) { $cG++ }
            if ($B -ge 254.0) { $cB++ }
            $sumL += $L; $sumSat += $sat; $sr += $R; $sg += $G; $sb += $B; $n++
        }
    }
    if ($n -eq 0) { return $null }
    [pscustomobject]@{
        n = $n
        luma = $sumL / $n
        sat  = $sumSat / $n
        clipPct = 100.0 * $clip / $n
        tint = ($sb - $sr) / $n          # + = toward blue/cyan, - = toward red/amber
        shN = $bN[0]; shL = if ($bN[0]) { $bL[0]/$bN[0] } else { 0 }
        shS = if ($bN[0]) { $bSat[0]/$bN[0] } else { 0 }
        mdN = $bN[1]; mdL = if ($bN[1]) { $bL[1]/$bN[1] } else { 0 }
        mdS = if ($bN[1]) { $bSat[1]/$bN[1] } else { 0 }
        hiN = $bN[2]; hiL = if ($bN[2]) { $bL[2]/$bN[2] } else { 0 }
        hiS = if ($bN[2]) { $bSat[2]/$bN[2] } else { 0 }
        p50 = Get-Pct $hist $n 0.50
        p99 = Get-Pct $hist $n 0.99
        p999 = Get-Pct $hist $n 0.999
        p100 = $lMax
        cR = 100.0 * $cR / $n
        cG = 100.0 * $cG / $n
        cB = 100.0 * $cB / $n
    }
}

$dirs = Get-ChildItem $Root -Directory | Where-Object { $_.Name -match '^\d+$' } | Sort-Object { [int]$_.Name }
Write-Output ("{0,4} | {1,6} {2,6} {3,6} | {4,6} {5,6} {6,6} | {7,6} {8,6} | {9,6} {10,6} {11,6} | {12,6} {13,6} {14,6}" -f `
    'shot','madL','unpL','dL','madSat','unpSat','dSat','madClip','unpClip','madShL','unpShL','dShL','madHiL','unpHiL','dHiL')
$rows = @()
foreach ($dir in $dirs) {
    $m = Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match '^madvr' -or $_.Name -match '\.mkv_' } | Select-Object -First 1
    $u = Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -eq 'unplugged_frame.png' } | Select-Object -First 1
    if (-not $m -or -not $u) { Write-Output ("{0,4} | incomplete pair" -f $dir.Name); continue }
    $sm = Measure-Image $m.FullName
    $su = Measure-Image $u.FullName
    if (-not $sm -or -not $su) { Write-Output ("{0,4} | measure failed" -f $dir.Name); continue }
    $rows += [pscustomobject]@{ shot=[int]$dir.Name; m=$sm; u=$su }
    Write-Output ("{0,4} | {1,6:F2} {2,6:F2} {3,7:+0.00;-0.00;0.00} | {4,6:F3} {5,6:F3} {6,7:+0.000;-0.000;0.000} | {7,6:F3} {8,6:F3} | {9,6:F2} {10,6:F2} {11,7:+0.00;-0.00;0.00} | {12,6:F2} {13,6:F2} {14,7:+0.00;-0.00;0.00}" -f `
        $dir.Name, $sm.luma, $su.luma, ($su.luma-$sm.luma),
        $sm.sat, $su.sat, ($su.sat-$sm.sat),
        $sm.clipPct, $su.clipPct,
        $sm.shL, $su.shL, ($su.shL-$sm.shL),
        $sm.hiL, $su.hiL, ($su.hiL-$sm.hiL))
}

Write-Output ''
Write-Output '--- tint (B-R, + = cyan-ward) and band saturation, per shot ---'
Write-Output ("{0,4} | {1,7} {2,7} {3,7} | {4,6} {5,6} {6,6} | {7,6} {8,6} {9,6}" -f `
    'shot','madTint','unpTint','dTint','madMdS','unpMdS','dMdS','madHiS','unpHiS','dHiS')
foreach ($r in $rows) {
    Write-Output ("{0,4} | {1,8:+0.00;-0.00;0.00} {2,8:+0.00;-0.00;0.00} {3,8:+0.00;-0.00;0.00} | {4,6:F3} {5,6:F3} {6,7:+0.000;-0.000;0.000} | {7,6:F3} {8,6:F3} {9,7:+0.000;-0.000;0.000}" -f `
        $r.shot, $r.m.tint, $r.u.tint, ($r.u.tint-$r.m.tint),
        $r.m.mdS, $r.u.mdS, ($r.u.mdS-$r.m.mdS),
        $r.m.hiS, $r.u.hiS, ($r.u.hiS-$r.m.hiS))
}

Write-Output ''
Write-Output '--- highlight tail (luma percentiles) + per-channel clip, % of content pixels ---'
Write-Output ("{0,4} | {1,6} {2,6} {3,6} | {4,6} {5,6} {6,6} | {7,6} {8,6} | {9,7} {10,7} | {11,7} {12,7} | {13,7} {14,7}" -f `
    'shot','madP99','unpP99','dP99','madP999','unpP999','dP999','madMax','unpMax','madR','unpR','madG','unpG','madB','unpB')
foreach ($r in $rows) {
    Write-Output ("{0,4} | {1,6:F0} {2,6:F0} {3,6:+0.0;-0.0;0.0} | {4,6:F0} {5,6:F0} {6,6:+0.0;-0.0;0.0} | {7,6:F0} {8,6:F0} | {9,7:F3} {10,7:F3} | {11,7:F3} {12,7:F3} | {13,7:F3} {14,7:F3}" -f `
        $r.shot, $r.m.p99, $r.u.p99, ($r.u.p99-$r.m.p99),
        $r.m.p999, $r.u.p999, ($r.u.p999-$r.m.p999),
        $r.m.p100, $r.u.p100,
        $r.m.cR, $r.u.cR, $r.m.cG, $r.u.cG, $r.m.cB, $r.u.cB)
}

Write-Output ''
Write-Output '--- set-wide means of the deltas (unplugged minus madvr) ---'
if ($rows.Count) {
    function AvgDelta([string]$field) {
        $v = $rows | ForEach-Object { $_.u.$field - $_.m.$field }
        return ($v | Measure-Object -Average).Average
    }
    Write-Output ("  luma        {0,8:+0.00;-0.00;0.00} codes" -f (AvgDelta 'luma'))
    Write-Output ("  saturation  {0,8:+0.0000;-0.0000;0.0000}" -f (AvgDelta 'sat'))
    Write-Output ("  clip frac   {0,8:+0.000;-0.000;0.000} pp" -f (AvgDelta 'clipPct'))
    Write-Output ("  tint B-R    {0,8:+0.00;-0.00;0.00}" -f (AvgDelta 'tint'))
    Write-Output ("  shadow luma {0,8:+0.00;-0.00;0.00}" -f (AvgDelta 'shL'))
    Write-Output ("  mid luma    {0,8:+0.00;-0.00;0.00}" -f (AvgDelta 'mdL'))
    Write-Output ("  high luma   {0,8:+0.00;-0.00;0.00}" -f (AvgDelta 'hiL'))
    Write-Output ("  mid sat     {0,8:+0.0000;-0.0000;0.0000}" -f (AvgDelta 'mdS'))
    Write-Output ("  high sat    {0,8:+0.0000;-0.0000;0.0000}" -f (AvgDelta 'hiS'))
    Write-Output ("  p99 luma    {0,8:+0.0;-0.0;0.0} codes" -f (AvgDelta 'p99'))
    Write-Output ("  p99.9 luma  {0,8:+0.0;-0.0;0.0} codes" -f (AvgDelta 'p999'))
    Write-Output ("  clip R %    {0,8:+0.000;-0.000;0.000} pp    G %  {1,8:+0.000;-0.000;0.000} pp    B %  {2,8:+0.000;-0.000;0.000} pp" -f `
        (AvgDelta 'cR'), (AvgDelta 'cG'), (AvgDelta 'cB'))

    function Get-Corr($xs, $ys) {
        $mx = ($xs | Measure-Object -Average).Average; $my = ($ys | Measure-Object -Average).Average
        $num = 0.0; $dx = 0.0; $dy = 0.0
        for ($i = 0; $i -lt $xs.Count; $i++) { $a = $xs[$i]-$mx; $b = $ys[$i]-$my; $num += $a*$b; $dx += $a*$a; $dy += $b*$b }
        if ($dx -gt 0 -and $dy -gt 0) { return $num / [math]::Sqrt($dx*$dy) } else { return 0 }
    }
    # does our saturation error grow with how saturated the scene already is?
    $c1 = Get-Corr @($rows | ForEach-Object { $_.m.sat }) @($rows | ForEach-Object { $_.u.sat - $_.m.sat })
    Write-Output ("  corr(scene sat, sat delta)   = {0:+0.00;-0.00;0.00}   (positive = we diverge more on saturated scenes)" -f $c1)
    # do we exaggerate whatever tint the scene already has (rather than adding a fixed tint)?
    $c2 = Get-Corr @($rows | ForEach-Object { [math]::Abs($_.m.tint) }) @($rows | ForEach-Object { [math]::Abs($_.u.tint) - [math]::Abs($_.m.tint) })
    Write-Output ("  corr(|madvr tint|, tint gain) = {0:+0.00;-0.00;0.00}   (positive = we amplify the scene's own tint)" -f $c2)
}
