# skin_p_by_band.ps1 -- what P(skin) does the trained LUT actually assign to each
# luminance band of a face? Uses the shader's OWN derivation (ps_ml_gamma.hlsl:217-224):
#   g22 = code^(1/2.2) per channel;  y = luma(g22);  cr = (g22.r - y)/1.5748;
#   cb = (g22.b - y)/1.8556;  uv = normalise(cr,cb) over the LUT bounds;  P = lut(uv).
# Settles whether a chroma-LUT guard can protect the highlight band at all, or whether
# the flooded pixels fall out of the classifier's skin region as radius collapses.
param([string]$Path = 'C:\Users\satee\Pictures\void\unplugged_frame_furiosa_default.png')
Add-Type -AssemblyName System.Drawing
$b0 = [System.IO.File]::ReadAllBytes('C:\code\unplugged-VideoRenderer\Source\res\skin_chroma_lut.bin')
$cr0=[BitConverter]::ToSingle($b0,0); $cr1=[BitConverter]::ToSingle($b0,4)
$cb0=[BitConverter]::ToSingle($b0,8); $cb1=[BitConverter]::ToSingle($b0,12)
$N=[BitConverter]::ToUInt16($b0,16)
$lut = New-Object 'double[,]' $N,$N
for($j=0;$j -lt $N;$j++){ for($i=0;$i -lt $N;$i++){ $lut[$i,$j] = $b0[20 + $j*$N + $i]/255.0 } }

$bmp = New-Object System.Drawing.Bitmap($Path)
$BW=$bmp.Width; $BH=$bmp.Height
$r = New-Object System.Drawing.Rectangle(0,0,$BW,$BH)
$d = $bmp.LockBits($r,[System.Drawing.Imaging.ImageLockMode]::ReadOnly,[System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$px = New-Object byte[] ($d.Stride*$BH); [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0,$px,0,$px.Length)
$bmp.UnlockBits($d); $bmp.Dispose()
$S=$d.Stride
$fx0=[int](1340*$BW/3840); $fx1=[int](2210*$BW/3840); $fy0=[int](480*$BH/2160); $fy1=[int](1440*$BH/2160)

$bands = [ordered]@{ '<=48 shadow'=@(0,48); '49-96'=@(49,96); '97-160 diffuse'=@(97,160); '161-210 sheen'=@(161,210); '>210 specular'=@(211,255) }
$acc = @{}; foreach($k in $bands.Keys){ $acc[$k] = @{n=0; p=0.0; rad=0.0; hi=0} }
for($y=$fy0+1;$y -lt $fy1;$y+=3){ for($x=$fx0+1;$x -lt $fx1;$x+=3){
    $o=$y*$S+$x*3
    $Bc=$px[$o]/255.0; $Gc=$px[$o+1]/255.0; $Rc=$px[$o+2]/255.0
    $lum8 = 255.0*($Rc*0.2126 + $Gc*0.7152 + $Bc*0.0722)
    foreach($k in $bands.Keys){ if($lum8 -ge $bands[$k][0] -and $lum8 -le $bands[$k][1]){
        $gR=[math]::Pow($Rc,1/2.2); $gG=[math]::Pow($Gc,1/2.2); $gB=[math]::Pow($Bc,1/2.2)
        $gy = $gR*0.2126 + $gG*0.7152 + $gB*0.0722
        $cr = ($gR - $gy)/1.5748; $cb = ($gB - $gy)/1.8556
        $u = ($cr-$cr0)/($cr1-$cr0); $v = ($cb-$cb0)/($cb1-$cb0)
        $p = 0.0
        if($u -ge 0 -and $u -le 1 -and $v -ge 0 -and $v -le 1){
            $p = $lut[[int]($u*($N-1)),[int]($v*($N-1))]
        }
        $a=$acc[$k]; $a.n++; $a.p += $p; $a.rad += [math]::Sqrt($cr*$cr+$cb*$cb)
        if($p -gt 0.5){ $a.hi++ }
        break
    }}
}}
Write-Output ("{0}`nface region sampled from {1}x{2}" -f (Split-Path $Path -Leaf), $BW, $BH)
Write-Output 'band              n      mean P(skin)   mean chroma r   %px with P>0.5'
foreach($k in $bands.Keys){ $a=$acc[$k]
    if($a.n -eq 0){ continue }
    Write-Output ("  {0,-16} {1,7}   {2,7:F3}        {3,7:F4}       {4,5:F1}%" -f `
        $k, $a.n, ($a.p/$a.n), ($a.rad/$a.n), (100.0*$a.hi/$a.n))
}
