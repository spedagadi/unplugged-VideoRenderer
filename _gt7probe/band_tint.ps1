# band_tint.ps1 -- where does the colour go wrong, and in which direction?
#
# Averages are not enough for a tint: a cyan cast in a dusk scene lives in one part of
# the tonal range and a set-wide mean luma cannot see it, because the shadow mass of a
# dusk frame outnumbers the dusk SKY by an order of magnitude. So this walks the two
# frames in luma deciles and prints, per decile, the mean R/G/B, the two axes a cast
# actually shows up on (B-R = blue/cyan vs amber; G-R = green vs magenta) and the
# per-channel clip fraction, which is the mechanism behind a hue that drifts UP rather
# than a hue that is simply wrong.
#
# Both files are read with the SAME sampler and the SAME decile edges, and the deciles are
# cut on MADVR's luma, then applied to both. Why that matters: if you cut each image on
# its own histogram, a renderer that delivers the scene 36 codes brighter puts the same
# cloud into a different band and the table compares a shadow with a highlight. The
# reference's bands are the question being asked, so they are the ruler.
param(
    [string]$Mad = 'C:\Users\satee\Pictures\Benchmarks\Dune\12\Dune.2021.German.TrueHD.Atmos.DL.2160p.UHD.BluRay.DV.HDR.HEVC.Remux-NIMA4K.mkv_20261011_135314.678.png',
    [string]$Unp = 'C:\Users\satee\Pictures\Benchmarks\Dune\12\unplugged_frame_2.png',
    [int]$Step = 4
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Get-Pixels([string]$path) {
    $im = New-Object System.Drawing.Bitmap($path)
    $w = $im.Width; $h = $im.Height
    $d = $im.LockBits((New-Object System.Drawing.Rectangle(0,0,$w,$h)),
                     [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                     [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $buf = New-Object byte[] ($d.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $buf.Length)
    $im.UnlockBits($d); $im.Dispose()
    return @{ buf = $buf; w = $w; h = $h; stride = $d.Stride }
}

# luma, then the per-channel clip counts, sampled every $Step pixels
function LumaOf($p, [int]$x, [int]$y) {
    $o = $y * $p.stride + $x * 3
    return 0.2126*$p.buf[$o+2] + 0.7152*$p.buf[$o+1] + 0.0722*$p.buf[$o]
}

$m = Get-Pixels $Mad
$u = Get-Pixels $Unp
"madvr    {0}x{1}" -f $m.w, $m.h
"unplugged{0}x{1}" -f $u.w, $u.h

# 1) decile edges on madVR's luma, over content pixels only (letterbox excluded)
$vals = New-Object System.Collections.Generic.List[double]
for ($y = 0; $y -lt $m.h; $y += $Step) {
    for ($x = 0; $x -lt $m.w; $x += $Step) {
        $L = LumaOf $m $x $y
        if ($L -ge 6.0) { $vals.Add($L) }
    }
}
$vals.Sort()
$n = $vals.Count
$edges = @()
for ($i = 1; $i -lt 10; $i++) { $edges += $vals[[int]($n * $i / 10)] }
$edges = @(0.0) + $edges + @(256.0)
"content pixels sampled: $n"

# 2) stats per band for both frames
function BandStats($p, [double]$lo, [double]$hi) {
    $c = 0; $sr = 0.0; $sg = 0.0; $sb = 0.0; $sl = 0.0
    $cr = 0; $cg = 0; $cb = 0; $sat = 0.0
    $ys = $p.h; if ($hi -ge $p.h) { $ys = $p.h }
    for ($y = 0; $y -lt $p.h; $y += $Step) {
        for ($x = 0; $x -lt $p.w; $x += $Step) {
            $L = LumaOf $p $x $y
            if ($L -lt $lo -or $L -ge $hi) { continue }
            if ($L -lt 6.0) { continue }
            $o = $y * $p.stride + $x * 3
            $B = [double]$p.buf[$o]; $G = [double]$p.buf[$o+1]; $R = [double]$p.buf[$o+2]
            $sr += $R; $sg += $G; $sb += $B; $sl += $L; $c++
            $mx = [Math]::Max($R,[Math]::Max($G,$B)); $mn = [Math]::Min($R,[Math]::Min($G,$B))
            if ($mx -gt 0) { $sat += ($mx-$mn)/$mx }
            if ($R -ge 254) { $cr++ }
            if ($G -ge 254) { $cg++ }
            if ($B -ge 254) { $cb++ }
        }
    }
    if ($c -eq 0) { return $null }
    [pscustomobject]@{ n=$c; L=$sl/$c; R=$sr/$c; G=$sg/$c; B=$sb/$c; sat=$sat/$c
                       cR=100.0*$cr/$c; cG=100.0*$cg/$c; cB=100.0*$cb/$c }
}

''
"{0,4} {1,13} | {2,5} {3,5} {4,5} {5,6} {6,6} | {7,5} {8,5} {9,5} {10,6} {11,6} | {12,6} {13,6} {14,6}" -f `
  'band','luma range','mL','mR','mG','mB-R','mS','uL','uR','uG','uB-R','uS','d(B-R)','dL','dB-R'
for ($i = 0; $i -lt 10; $i++) {
    $a = BandStats $m $edges[$i] $edges[$i+1]
    $b = BandStats $u $edges[$i] $edges[$i+1]
    if (-not $a -or -not $b) { continue }
    $tag = if ($i -eq 0) { 'D1 dark' } elseif ($i -eq 4) { 'D5 mid' } elseif ($i -eq 9) { 'D10 top' } else { "D$($i+1)" }
    "{0,4} {1,5:F0}-{2,5:F0} | {3,5:F1} {4,5:F1} {5,5:F1} {6,6:F1} {7,6:F3} | {8,5:F1} {9,5:F1} {10,5:F1} {11,6:F1} {12,6:F3} | {13,6:+0.0;-0.0;0.0} {14,6:+0.0;-0.0;0.0} {15,6:+0.0;-0.0;0.0}" -f `
      $tag, $edges[$i], $edges[$i+1], $a.L, $a.R, $a.G, ($a.B-$a.R), $a.sat, $b.L, $b.R, $b.G, ($b.B-$b.R), $b.sat, `
      (($b.B-$b.R)-($a.B-$a.R)), ($b.L-$a.L), ($b.B-$a.B)
}
''
'--- per-channel clip, % of band (the mechanism for a hue that drifts UP) ---'
"{0,4} | {1,7} {2,7} | {3,7} {4,7} | {5,7} {6,7}" -f 'band','madR','unpR','madG','unpG','madB','unpB'
for ($i = 0; $i -lt 10; $i++) {
    $a = BandStats $m $edges[$i] $edges[$i+1]
    $b = BandStats $u $edges[$i] $edges[$i+1]
    if (-not $a -or -not $b) { continue }
    $tag = if ($i -eq 0) { 'D1' } elseif ($i -eq 9) { 'D10' } else { "D$($i+1)" }
    "{0,4} | {1,7:F3} {2,7:F3} | {3,7:F3} {4,7:F3} | {5,7:F3} {6,7:F3}" -f `
      $tag, $a.cR, $b.cR, $a.cG, $b.cG, $a.cB, $b.cB
}
