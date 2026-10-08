# render_compare.ps1 -- compare two graded renders pixel-for-pixel: does the shadow
# dial change COLOUR (saturation / hue), and does it change TEXTURE (local luma
# gradient) inside the shadow band? "Plasticky" is a texture claim; "changing pixel
# colour" is a chroma claim. Both are measurable; neither needs an opinion.
param(
    [string]$Base = 'C:\Users\satee\Pictures\Furiosa\unplugged_frame_sh0.png',
    [string]$Test = 'C:\Users\satee\Pictures\Furiosa\unplugged_frame_sh5.png',
    [int]$Stride = 4
)
Add-Type -AssemblyName System.Drawing

function Load([string]$path) {
    if (-not (Test-Path $path)) { throw "missing: $path" }
    $bmp = New-Object System.Drawing.Bitmap($path)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $bytes = $data.Stride * $bmp.Height
    $buf = New-Object byte[] $bytes
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $bytes)
    $bmp.UnlockBits($data)
    $out = [pscustomobject]@{ W = $bmp.Width; H = $bmp.Height; Stride = $data.Stride; Pix = $buf }
    $bmp.Dispose()
    return $out
}

function Hue([double]$r,[double]$g,[double]$b) {
    $mx = [math]::Max($r, [math]::Max($g, $b)); $mn = [math]::Min($r, [math]::Min($g, $b))
    $d = $mx - $mn
    if ($d -le 0.5) { return -1.0 }
    $h = 0.0
    if ($mx -eq $r) { $h = 60.0 * ((($g - $b) / $d) % 6.0) }
    elseif ($mx -eq $g) { $h = 60.0 * ((($b - $r) / $d) + 2.0) }
    else { $h = 60.0 * ((($r - $g) / $d) + 4.0) }
    if ($h -lt 0) { $h += 360.0 }
    return $h
}

$a = Load $Base
$b = Load $Test
Write-Output ("base {0}x{1}   test {2}x{3}" -f $a.W,$a.H,$b.W,$b.H)
if ($a.W -ne $b.W -or $a.H -ne $b.H) { throw 'different sizes -- pixel comparison invalid' }

$Wl = 0.2126; $Wg = 0.7152; $Wb = 0.0722
function LumaOf($im, [int]$x, [int]$y) {
    $o = $y * $im.Stride + $x * 3
    return $im.Pix[$o]*$Wb + $im.Pix[$o+1]*$Wg + $im.Pix[$o+2]*$Wl   # B,G,R in 24bpp
}

$n = 0; $nHue = 0
$satA = 0.0; $satB = 0.0; $lumA = 0.0; $lumB = 0.0
$hueA = 0.0; $hueB = 0.0; $hueDiff = 0.0
$texA = 0.0; $texB = 0.0; $texCount = 0
# midtone control band, so a texture change in shadows is not read as a global effect
$mSatA = 0.0; $mSatB = 0.0; $mTexA = 0.0; $mTexB = 0.0; $mN = 0

for ($y = 2; $y -lt $a.H - 2; $y += $Stride) {
    for ($x = 2; $x -lt $a.W - 2; $x += $Stride) {
        $la = LumaOf $a $x $y
        if ($la -le 4.0 -or $la -gt 48.0) {
            if ($la -gt 96.0 -and $la -le 160.0) {
                # midtone control
                $oa = $y*$a.Stride + $x*3; $ob = $y*$b.Stride + $x*3
                $r1=$a.Pix[$oa+2]; $g1=$a.Pix[$oa+1]; $b1=$a.Pix[$oa]
                $r2=$b.Pix[$ob+2]; $g2=$b.Pix[$ob+1]; $b2=$b.Pix[$ob]
                $mx1=[math]::Max($r1,[math]::Max($g1,$b1)); $mn1=[math]::Min($r1,[math]::Min($g1,$b1))
                $mx2=[math]::Max($r2,[math]::Max($g2,$b2)); $mn2=[math]::Min($r2,[math]::Min($g2,$b2))
                if ($mx1 -gt 0) { $mSatA += ($mx1-$mn1)/$mx1 }
                if ($mx2 -gt 0) { $mSatB += ($mx2-$mn2)/$mx2 }
                $gx1=[math]::Abs((LumaOf $a ($x+1) $y) - (LumaOf $a ($x-1) $y))
                $gy1=[math]::Abs((LumaOf $a $x ($y+1)) - (LumaOf $a $x ($y-1)))
                $gx2=[math]::Abs((LumaOf $b ($x+1) $y) - (LumaOf $b ($x-1) $y))
                $gy2=[math]::Abs((LumaOf $b $x ($y+1)) - (LumaOf $b $x ($y-1)))
                $mTexA += ($gx1+$gy1); $mTexB += ($gx2+$gy2); $mN++
            }
            continue
        }
        $oa = $y*$a.Stride + $x*3; $ob = $y*$b.Stride + $x*3
        $r1=[double]$a.Pix[$oa+2]; $g1=[double]$a.Pix[$oa+1]; $b1=[double]$a.Pix[$oa]
        $r2=[double]$b.Pix[$ob+2]; $g2=[double]$b.Pix[$ob+1]; $b2=[double]$b.Pix[$ob]
        $mx1=[math]::Max($r1,[math]::Max($g1,$b1)); $mn1=[math]::Min($r1,[math]::Min($g1,$b1))
        $mx2=[math]::Max($r2,[math]::Max($g2,$b2)); $mn2=[math]::Min($r2,[math]::Min($g2,$b2))
        if ($mx1 -gt 0) { $satA += ($mx1-$mn1)/$mx1 }
        if ($mx2 -gt 0) { $satB += ($mx2-$mn2)/$mx2 }
        $h1 = Hue $r1 $g1 $b1; $h2 = Hue $r2 $g2 $b2
        if ($h1 -ge 0 -and $h2 -ge 0) {
            $d = [math]::Abs($h1-$h2); if ($d -gt 180) { $d = 360-$d }
            if ($d -gt 2.0) { $nHue++ }
            $hueDiff += $d; $hueA += $h1; $hueB += $h2
        }
        $lumA += $la; $lumB += (LumaOf $b $x $y)
        $gx1=[math]::Abs((LumaOf $a ($x+1) $y) - (LumaOf $a ($x-1) $y))
        $gy1=[math]::Abs((LumaOf $a $x ($y+1)) - (LumaOf $a $x ($y-1)))
        $gx2=[math]::Abs((LumaOf $b ($x+1) $y) - (LumaOf $b ($x-1) $y))
        $gy2=[math]::Abs((LumaOf $b $x ($y+1)) - (LumaOf $b $x ($y-1)))
        $texA += ($gx1+$gy1); $texB += ($gx2+$gy2); $texCount++
        $n++
    }
}

Write-Output ''
Write-Output ("shadow band (4,48] sampled: {0} px   (every {1}th)" -f $n, $Stride)
Write-Output ("  mean luma      base {0,7:F2}   test {1,7:F2}   delta {2,7:F2}" -f ($lumA/$n), ($lumB/$n), (($lumB-$lumA)/$n))
Write-Output ("  mean saturation base {0,7:F4}  test {1,7:F4}  delta {2,7:F4}" -f ($satA/$n), ($satB/$n), (($satB-$satA)/$n))
Write-Output ("  mean hue angle   base {0,7:F2}   test {1,7:F2}   mean per-pixel shift {2:F2} deg" -f ($hueA/$n), ($hueB/$n), ($hueDiff/$n))
Write-Output ("  pixels with hue shift > 2 deg: {0} of {1} ({2:F1}%)" -f $nHue, $n, (100.0*$nHue/$n))
Write-Output ("  TEXTURE (mean |grad| luma) base {0,7:F3}  test {1,7:F3}  ratio {2,7:F3}" -f ($texA/$texCount), ($texB/$texCount), ($texB/$texA))
Write-Output ''
Write-Output ("midtone control (96,160] sampled: {0} px" -f $mN)
if ($mN -gt 0) {
    Write-Output ("  mean saturation base {0,7:F4}  test {1,7:F4}  delta {2,7:F4}" -f ($mSatA/$mN), ($mSatB/$mN), (($mSatB-$mSatA)/$mN))
    Write-Output ("  TEXTURE            base {0,7:F3}  test {1,7:F3}  ratio {2,7:F3}" -f ($mTexA/$mN), ($mTexB/$mN), ($mTexB/$mTexA))
}

# ---------------------------------------------------------------------------
# Does the CHANNEL RATIO survive? Hue-preservation in linear light is only worth
# claiming if it is still true in the delivered, encoded, graded frame. Hue ANGLE
# is a bad instrument here: in the shadow band chroma is low, so a couple of code
# values of noise moves the angle by degrees without any visible colour change.
# The normalised channel r = R/(R+G+B) is the honest measure -- it is exactly what
# a luminance-only operator must leave alone.
# ---------------------------------------------------------------------------
$rr1 = New-Object System.Collections.Generic.List[double]
$gg1 = New-Object System.Collections.Generic.List[double]
$bb1 = New-Object System.Collections.Generic.List[double]
$rat = New-Object System.Collections.Generic.List[double]
for ($y = 2; $y -lt $a.H - 2; $y += $Stride) {
    for ($x = 2; $x -lt $a.W - 2; $x += $Stride) {
        $la = LumaOf $a $x $y
        if ($la -le 4.0 -or $la -gt 48.0) { continue }
        $oa = $y*$a.Stride + $x*3; $ob = $y*$b.Stride + $x*3
        $R1=[double]$a.Pix[$oa+2]; $G1=[double]$a.Pix[$oa+1]; $B1=[double]$a.Pix[$oa]
        $R2=[double]$b.Pix[$ob+2]; $G2=[double]$b.Pix[$ob+1]; $B2=[double]$b.Pix[$ob]
        $s1 = $R1+$G1+$B1; $s2 = $R2+$G2+$B2
        if ($s1 -lt 6 -or $s2 -lt 6) { continue }        # near-black: ratios are noise
        $rr1.Add([math]::Abs($R1/$s1 - $R2/$s2))
        $gg1.Add([math]::Abs($G1/$s1 - $G2/$s2))
        $bb1.Add([math]::Abs($B1/$s1 - $B2/$s2))
        $rat.Add(($R2+$G2+$B2)/($R1+$G1+$B1))
    }
}
function Pct($list, [double]$p) {
    $s = $list | Sort-Object
    return $s[[int]([math]::Min($s.Count-1, [math]::Round($p*($s.Count-1))))]
}
$meanRR = ($rr1 | Measure-Object -Average).Average
$meanGG = ($gg1 | Measure-Object -Average).Average
$meanBB = ($bb1 | Measure-Object -Average).Average
Write-Output ''
Write-Output ("channel-ratio drift in the shadow band, n={0} (0.000 = colour untouched)" -f $rr1.Count)
Write-Output ("  mean |d(R/s)| {0:F4}   |d(G/s)| {1:F4}   |d(B/s)| {2:F4}" -f $meanRR, $meanGG, $meanBB)
Write-Output ("  p90  {0:F4}  p99 {1:F4}   (expressed in normalised units; 0.004 ~ 1 code at sum=255)" -f (Pct $rr1 0.90), (Pct $rr1 0.99))
Write-Output ("  per-pixel total shift: >0.010 = {0:F1}%   >0.020 = {1:F1}%" -f `
    (100.0*($rr1 | Where-Object { $_ -gt 0.010 }).Count/$rr1.Count), `
    (100.0*($rr1 | Where-Object { $_ -gt 0.020 }).Count/$rr1.Count))
Write-Output ''
Write-Output ("luminance gain actually delivered in the band: mean {0:F2}x  p10 {1:F2}x  p50 {2:F2}x  p90 {3:F2}x" -f `
    (($rat | Measure-Object -Average).Average), (Pct $rat 0.10), (Pct $rat 0.50), (Pct $rat 0.90))
