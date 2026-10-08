# face_sheen.ps1 -- characterise the skin-highlight band in a frame: where does the
# "glossy" energy sit in code terms, and how much local micro-contrast does it carry?
# Answers two things: (1) can the shadow dial (band <=48) be responsible at all, and
# (2) is the residual sheen a CHROMA problem or a LUMINANCE-micro-contrast problem.
param([string]$Path = 'C:\Users\satee\Pictures\void\unplugged_frame_furiosa.png')
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($Path)
$rect = New-Object System.Drawing.Rectangle(0,0,$bmp.Width,$bmp.Height)
$d = $bmp.LockBits($rect,[System.Drawing.Imaging.ImageLockMode]::ReadOnly,[System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$buf = New-Object byte[] ($d.Stride*$bmp.Height)
[System.Runtime.InteropServices.Marshal]::Copy($d.Scan0,$buf,0,$buf.Length)
$bmp.UnlockBits($d)
# Capture the size BEFORE Dispose -- reading $bmp.Width after disposal returns 0 and
# silently collapses the sample box to nothing, which is how this script's first run
# reported "0 px" and an empty table instead of failing loudly.
$BW=$bmp.Width; $BH=$bmp.Height
$bmp.Dispose()
$W=$d.Stride

# Face box, scaled to this image's size (measured on the 3840x2160 Furiosa frame).
$fx0=[int](1340*$BW/3840); $fx1=[int](2210*$BW/3840)
$fy0=[int](480*$BH/2160); $fy1=[int](1440*$BH/2160)
function Lum([int]$x,[int]$y){ $o=$y*$W+$x*3; return $buf[$o]*0.0722 + $buf[$o+1]*0.7152 + $buf[$o+2]*0.2126 }

# Luma histogram of the face region, bucketed by band.
$bands = [ordered]@{ 'shadow <=48'=@(0,48); 'lower-mid 49-96'=@(49,96); 'skin diffuse 97-160'=@(97,160); 'skin sheen 161-210'=@(161,210); 'specular >210'=@(211,255) }
$tot=0; $counts=@{}
foreach($k in $bands.Keys){ $counts[$k]=0 }
for($y=$fy0;$y -lt $fy1;$y+=2){ for($x=$fx0;$x -lt $fx1;$x+=2){
    $l=Lum $x $y; $tot++
    foreach($k in $bands.Keys){ if($l -ge $bands[$k][0] -and $l -le $bands[$k][1]){ $counts[$k]++; break } }
}}
Write-Output ("face region {0}x{1}, sampled {2} px" -f ($fx1-$fx0),($fy1-$fy0),$tot)
Write-Output 'band                        share   mean-sat   mean-local-contrast'
foreach($k in $bands.Keys){
    $sat=0.0; $con=0.0; $n=0
    for($y=$fy0+1;$y -lt $fy1-1;$y+=3){ for($x=$fx0+1;$x -lt $fx1-1;$x+=3){
        $l=Lum $x $y
        if($l -ge $bands[$k][0] -and $l -le $bands[$k][1]){
            $o=$y*$W+$x*3; $mx=[math]::Max($buf[$o+2],[math]::Max($buf[$o+1],$buf[$o])); $mn=[math]::Min($buf[$o+2],[math]::Min($buf[$o+1],$buf[$o]))
            if($mx -gt 0){ $sat += ($mx-$mn)/$mx }
            $g=[math]::Abs((Lum ($x+1) $y)-(Lum ($x-1) $y)) + [math]::Abs((Lum $x ($y+1))-(Lum $x ($y-1)))
            $con += $g; $n++
        }
    }}
    if($n -gt 0){
        Write-Output ("  {0,-24} {1,5:F1}%  {2,7:F3}   {3,7:F2}" -f $k, (100.0*$counts[$k]/$tot), ($sat/$n), ($con/$n))
    }
}
