# fill_by_hue.ps1 -- where does madVR actually put a saturated colour, by HUE?
#
# The question this exists to answer: the dusk sky (Dune 12) sits at gamut fill 0.80 where
# madVR sits it at 0.41, and the yellow graphic the user wants left punchy sits at fill
# 1.00. Lightness does not separate them, and "is it inside the triangle" does not either.
# So before building a hue-aware rule, measure whether madVR HAS one: take every bright,
# saturated pixel in a matched pair, compute the gamut fill the renderer actually delivered
# (1.0 = pinned on the BT.709 edge, 0.5 = half of the available chroma), and bucket the
# answer by hue.
#
#   - if madVR holds AMBER/YELLOW high and pulls CYAN/BLUE low, the discriminator is real
#     and belongs in the constraint;
#   - if madVR pulls every hue to roughly the same fill, then it is only ever the
#     lightness/level rule, and the bug is simply that our relaxation band does not match
#     the anchors documented in colorspace_gamut_conversion.hlsl.
#
# Fill is computed with the shader's OWN functions (GamutEdgeScale / the luma weights) on
# LINEAR values, because that is the domain the constraint works in -- measuring it on
# encoded codes is the I-vs-chroma domain split that has made saturation drift three times
# already in this project.
param(
    [string]$Root = 'C:\Users\satee\Pictures\Benchmarks\Bladerunner',
    [int]$Step = 6,
    [double]$MinSat = 0.35,
    [double]$MinY = 0.12
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Edge-Scale([double]$ch, [double]$lum) {
    if ($ch -lt $lum) { $d = $lum - $ch; if ($d -gt 1e-6) { return $lum / $d } else { return 1e6 } }
    $d2 = $ch - $lum
    if ($d2 -lt 1e-6) { return 1e6 }
    return (1.0 - $lum) / $d2
}

function Hue-Sector([double]$rf, [double]$gf, [double]$bf) {
    $mx = [Math]::Max($rf, [Math]::Max($gf, $bf)); $mn = [Math]::Min($rf, [Math]::Min($gf, $bf))
    $dd = $mx - $mn; if ($dd -lt 1e-6) { return -1 }
    $ang = 0.0
    if ($mx -eq $rf) { $ang = 60.0 * (($gf - $bf) / $dd) }
    elseif ($mx -eq $gf) { $ang = 60.0 * (2.0 + ($bf - $rf) / $dd) }
    else { $ang = 60.0 * (4.0 + ($rf - $gf) / $dd) }
    if ($ang -lt 0) { $ang += 360.0 }
    # sector names ordered around the wheel, warm first (that is the axis in dispute)
    if ($ang -lt 15 -or $ang -ge 345) { return 0 }   # red
    if ($ang -lt 45)  { return 1 }                    # amber / orange
    if ($ang -lt 75)  { return 2 }                    # yellow
    if ($ang -lt 135) { return 3 }                    # green
    if ($ang -lt 195) { return 4 }                    # CYAN  (the complaint)
    if ($ang -lt 255) { return 5 }                    # blue
    return 6                                          # magenta / purple
}
$SectorName = @('red', 'amber', 'yellow', 'green', 'CYAN', 'blue', 'magenta')

function Collect-Fills([string]$path) {
    $bm = New-Object System.Drawing.Bitmap($path)
    $wd = $bm.Width; $ht = $bm.Height
    $d = $bm.LockBits((New-Object System.Drawing.Rectangle(0,0,$wd,$ht)),
                     [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                     [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $buf = New-Object byte[] ($d.Stride * $ht)
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length)
    $bm.UnlockBits($d); $bm.Dispose()
    $st = $d.Stride
    $perSector = @{}
    for ($i = 0; $i -lt 7; $i++) { $perSector[$i] = New-Object System.Collections.Generic.List[double] }
    for ($y = 0; $y -lt $ht; $y += $Step) {
        $rowBase = $y * $st
        for ($x = 0; $x -lt $wd; $x += $Step) {
            $o = $rowBase + $x * 3
            $bl = [double]$buf[$o] / 255.0; $gl = [double]$buf[$o+1] / 255.0; $rl = [double]$buf[$o+2] / 255.0
            $mx = [Math]::Max($rl, [Math]::Max($gl, $bl)); $mn = [Math]::Min($rl, [Math]::Min($gl, $bl))
            if ($mx -le 0) { continue }
            $sat = ($mx - $mn) / $mx
            if ($sat -lt $MinSat) { continue }
            # linear light for the geometry, encoded for nothing
            $lR = [Math]::Pow($rl, 2.2); $lG = [Math]::Pow($gl, 2.2); $lB = [Math]::Pow($bl, 2.2)
            $lum = 0.2126*$lR + 0.7152*$lG + 0.0722*$lB
            if ($lum -lt $MinY -or $lum -gt 0.98) { continue }
            $sec = Hue-Sector $rl $gl $bl
            if ($sec -lt 0) { continue }
            $sScale = [Math]::Min([Math]::Min((Edge-Scale $lR $lum), (Edge-Scale $lG $lum)), (Edge-Scale $lB $lum))
            $perSector[$sec].Add(1.0 / $sScale)      # 1.0 = pinned on the 709 edge
        }
    }
    return $perSector
}

function Median($list) {
    if ($list.Count -eq 0) { return $null }
    $arr = $list.ToArray(); [Array]::Sort($arr)
    return $arr[[int]($arr.Count / 2)]
}

$dirs = Get-ChildItem $Root -Directory | Where-Object { $_.Name -match '^\d+$' } | Sort-Object { [int]$_.Name }
$aggMad = @{}; $aggUnp = @{}
for ($i = 0; $i -lt 7; $i++) { $aggMad[$i] = New-Object System.Collections.Generic.List[double]; $aggUnp[$i] = New-Object System.Collections.Generic.List[double] }

foreach ($dir in $dirs) {
    $madFile = Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match 'madvr|\.mkv_' } | Select-Object -First 1
    $unpFile = Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match 'unplugged' } | Select-Object -First 1
    if (-not $madFile -or -not $unpFile) { "  skip $($dir.Name) (no pair)"; continue }
    $cm = Collect-Fills $madFile.FullName
    $cu = Collect-Fills $unpFile.FullName
    $parts = @()
    for ($i = 0; $i -lt 7; $i++) {
        if ($cm[$i].Count -ge 30) { $aggMad[$i].AddRange($cm[$i]) }
        if ($cu[$i].Count -ge 30) { $aggUnp[$i].AddRange($cu[$i]) }
        $mv = Median $cm[$i]; $uv = Median $cu[$i]
        if ($mv) { $parts += ("{0} m={1:F2}/u={2:F2}({3})" -f $SectorName[$i], $mv, $uv, $cm[$i].Count) }
    }
    "{0,3}  {1}" -f $dir.Name, ($parts -join '  ')
}

''
'=== AGGREGATE median delivered gamut fill (1.00 = pinned on the 709 edge) ==='
'{0,-9} {1,7} {2,7} {3,7} {4,6} {5,7}' -f 'sector','madVR','unplug','delta','nMad','ratio'
for ($i = 0; $i -lt 7; $i++) {
    $mm = Median $aggMad[$i]; $mu = Median $aggUnp[$i]
    if (-not $mm) { continue }
    '{0,-9} {1,7:F3} {2,7:F3} {3,7:+0.000;-0.000;0.000} {4,6} {5,6:F2}' -f `
        $SectorName[$i], $mm, $mu, ($mu - $mm), $aggMad[$i].Count, ($(if ($mu) { $mu / $mm } else { 0 }))
}
''
'interpretation: madVR holds a sector near 1.0 -> it leaves that hue alone when it fits,'
'and pulls a sector low -> it treats that hue as un-showable even when legal.'
