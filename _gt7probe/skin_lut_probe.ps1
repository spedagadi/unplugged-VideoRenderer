# skin_lut_probe.ps1 -- read the trained P(skin) chroma LUT and answer one question:
# does it still call a pixel skin once that pixel has been brightened toward white?
# Brightening under a scalar gain preserves HUE and destroys CHROMA MAGNITUDE, so a
# classifier indexed on (Cr,Cb) position loses skin exactly in the highlight band.
$ErrorActionPreference = 'Stop'
$path = 'C:\code\unplugged-VideoRenderer\Source\res\skin_chroma_lut.bin'
$b = [System.IO.File]::ReadAllBytes($path)
$cr0 = [BitConverter]::ToSingle($b, 0); $cr1 = [BitConverter]::ToSingle($b, 4)
$cb0 = [BitConverter]::ToSingle($b, 8); $cb1 = [BitConverter]::ToSingle($b, 12)
$N = [BitConverter]::ToUInt16($b, 16); $N2 = [BitConverter]::ToUInt16($b, 18)
Write-Output ("LUT {0}x{1} (square={2})  Cr[{3:F4},{4:F4}]  Cb[{5:F4},{6:F4}]  bytes={7}" -f `
    $N, $N2, ($N -eq $N2), $cr0, $cr1, $cb0, $cb1, $b.Length)
$px = New-Object 'double[,]' $N, $N
for ($j = 0; $j -lt $N; $j++) { for ($i = 0; $i -lt $N; $i++) {
    $px[$i, $j] = $b[20 + $j * $N + $i] / 255.0
}}
# peak
$best = 0.0; $bi = 0; $bj = 0
for ($j = 0; $j -lt $N; $j++) { for ($i = 0; $i -lt $N; $i++) { if ($px[$i,$j] -gt $best) { $best = $px[$i,$j]; $bi = $i; $bj = $j } } }
$crAt = $cr0 + ($bi / ($N - 1)) * ($cr1 - $cr0)
$cbAt = $cb0 + ($bj / ($N - 1)) * ($cb1 - $cb0)
# NOTE: computing this inline inside the -f argument list made PowerShell read the
# trailing "%" as the modulo operator applied to the formatted string.
$peakHue = [math]::Atan2($cbAt, $crAt) * 180.0 / [math]::PI
if ($peakHue -lt 0) { $peakHue += 360.0 }
$peakRad = [math]::Sqrt($crAt*$crAt + $cbAt*$cbAt)
Write-Output ("peak P(skin)={0:F2} at Cr={1:F4} Cb={2:F4}  (chroma radius {3:F4}, hue {4:F1} deg)" -f `
    $best, $crAt, $cbAt, $peakRad, $peakHue)

# For each hue, what does P(skin) look like as chroma shrinks toward neutral?
Write-Output ''
Write-Output 'P(skin) along the BEST hue, as chroma radius falls toward zero:'
Write-Output '  radius    P(skin)   (0 = classifier no longer claims the pixel)'
# best hue angle
$bestHue = (([math]::Atan2($cbAt, $crAt) * 180 / [math]::PI) + 360) % 360
foreach ($r in @(0.30,0.25,0.20,0.15,0.12,0.10,0.08,0.06,0.04,0.02,0.0)) {
    $rad = $r * [math]::PI / 180.0
    $cr = [math]::Cos($bestHue * [math]::PI / 180.0) * $r
    $cb = [math]::Sin($bestHue * [math]::PI / 180.0) * $r
    $u = ($cr - $cr0) / ($cr1 - $cr0); $v = ($cb - $cb0) / ($cb1 - $cb0)
    if ($u -lt 0 -or $u -gt 1 -or $v -lt 0 -or $v -gt 1) { Write-Output ("  {0,5:F3}    outside grid (u={1:F2} v={2:F2})" -f $r, $u, $v); continue }
    $x = $u * ($N - 1); $y = $v * ($N - 1)
    $x0 = [math]::Min($N-1, [math]::Floor($x)); $y0 = [math]::Min($N-1, [math]::Floor($y))
    $p = $px[$x0, $y0]
    Write-Output ("  {0,5:F3}    {1:F3}" -f $r, $p)
}

# hue coverage: for each hue, the max P over radius, and the P at a small radius
Write-Output ''
Write-Output 'by hue: maxP over all chroma, and P at low chroma (r=0.06) -- the highlight case'
for ($h = 0; $h -lt 360; $h += 15) {
    $maxP = 0.0; $lowP = 0.0
    foreach ($r in @(0.02,0.04,0.06,0.08,0.10,0.12,0.15,0.18,0.22,0.26,0.30)) {
        $rr = $r * [math]::PI / 180.0
        $cr = [math]::Cos($h * [math]::PI / 180.0) * $r
        $cb = [math]::Sin($h * [math]::PI / 180.0) * $r
        $u = ($cr - $cr0) / ($cr1 - $cr0); $v = ($cb - $cb0) / ($cb1 - $cb0)
        if ($u -lt 0 -or $u -gt 1 -or $v -lt 0 -or $v -gt 1) { continue }
        $p = $px[[math]::Min($N-1,[math]::Floor($u*($N-1))), [math]::Min($N-1,[math]::Floor($v*($N-1)))]
        $maxP = [math]::Max($maxP, $p)
        if ([math]::Abs($r - 0.06) -lt 1e-9) { $lowP = $p }
    }
    Write-Output ("  hue {0,3}deg   maxP {1:F3}   P@r=0.06 {2:F3}" -f $h, $maxP, $lowP)
}
