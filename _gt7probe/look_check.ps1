# look_check.ps1 -- verify ApplyLook v3 (luminance-domain) preserves hue and stays
# monotone, before trusting a build to it. Mirrors the HLSL arithmetic:
#   shape = shadow window, then mid-grey power, applied to Rec.709 luma in code space;
#   out   = RGB * (Y_out / Y_in), capped so no channel clips when brightening.
# Scalars in, object out -- no arrays, because array argument binding in this
# harness silently nests and turns $a[0]*2 into an op_Multiply failure.
$ErrorActionPreference = 'Stop'
$KW = 0.2126; $GW = 0.7152; $BW = 0.0722
# The project's OWN shadow band, from Source/ml_features.cpp:459-460 -- (4,48]/255.
# The dial and the DELIVERED shMean/shN columns must describe the same pixels.
$K  = 48.0/255.0
$CRUSH = 4.0/255.0

function Enc([double]$v) { if ($v -gt 0.0031308) { 1.055 * [math]::Pow($v, 1/2.4) - 0.055 } else { 12.92 * $v } }
function Dec([double]$v) { if ($v -gt 0.04045) { [math]::Pow(($v + 0.055)/1.055, 2.4) } else { $v/12.92 } }
function Luma([double]$r,[double]$g,[double]$b) { $r*$KW + $g*$GW + $b*$BW }

function Taste([double]$r,[double]$g,[double]$b,[double]$lift,[double]$punch) {
    $yIn = Luma $r $g $b
    if ($yIn -le 1e-6) { return [pscustomobject]@{R=$r;G=$g;B=$b} }
    if ([math]::Abs($lift) -lt 1e-4 -and [math]::Abs($punch) -lt 1e-4) { return [pscustomobject]@{R=$r;G=$g;B=$b} }
    $y = Enc $yIn
    $q = [math]::Pow(2, $lift * 0.6)     # authority matches the shader (measured 1.97x was too much)
    if ($y -lt $K) { $y = $K * (1 - [math]::Pow(1 - $y/$K, $q)) }
    $s = [math]::Pow(2, -$punch)
    $d = [math]::Abs($y - 0.5)
    $sgn = 0.0
    if ($y -gt 0.5) { $sgn = 1.0 } elseif ($y -lt 0.5) { $sgn = -1.0 }
    $y = [math]::Min(1.0, [math]::Max(0.0, 0.5 + $sgn * 0.5 * [math]::Pow([math]::Max(2*$d,0.0), $s)))
    $scale = (Dec $y) / $yIn
    $hi = [math]::Max($r, [math]::Max($g, $b))
    if ($scale -gt 1.0 -and $hi -gt 0.0) { $scale = [math]::Min($scale, 1.0/$hi) }
    return [pscustomobject]@{ R = $r*$scale; G = $g*$scale; B = $b*$scale }
}

Write-Output '== 1. HUE / SATURATION: normalised channels must not move =='
$labels = 'red face','sky','foliage','skin','deep blue','mid grey','near black'
# NOTE the variable names: PowerShell is case-INSENSITIVE, so an array called $R
# and a loop local called $r are the SAME variable -- assigning the element clobbers
# the array and the next iteration indexes a scalar. Cost me a run to find.
$chR = @(0.90,0.35,0.20,0.65,0.10,0.50,0.02)
$chG = @(0.30,0.55,0.60,0.45,0.15,0.50,0.05)
$chB = @(0.30,0.90,0.25,0.35,0.60,0.50,0.03)
$worst = 0.0
for ($c = 0; $c -lt $labels.Count; $c++) {
    $r=$chR[$c]; $g=$chG[$c]; $b=$chB[$c]
    $sIn = $r + $g + $b
    foreach ($t in @(-1,-0.6,0,0.6,1)) { foreach ($p in @(-1,-0.6,0,0.6,1)) {
        $o = Taste $r $g $b $t $p
        $sOut = $o.R + $o.G + $o.B
        if ($sOut -le 0) { continue }
        $worst = [math]::Max($worst, [math]::Abs($r/$sIn - $o.R/$sOut))
        $worst = [math]::Max($worst, [math]::Abs($g/$sIn - $o.G/$sOut))
        $worst = [math]::Max($worst, [math]::Abs($b/$sIn - $o.B/$sOut))
    }}
}
Write-Output ("  worst drift: {0:E2}   (zero = hue and saturation preserved at every dial setting)" -f $worst)

Write-Output '== 2. MONOTONICITY: delivered luma must never fall as a dial rises =='
foreach ($mode in @('shadow','punch')) {
    $prev = -1.0; $folds = 0
    for ($i = 0; $i -le 255; $i++) {
        $dial = ($i/255.0)*2 - 1
        $o = if ($mode -eq 'shadow') { Taste 0.25 0.45 0.30 $dial 0 } else { Taste 0.25 0.45 0.30 0 $dial }
        $y = Luma $o.R $o.G $o.B
        if ($y -lt $prev - 1e-9) { $folds++ }
        $prev = $y
    }
    Write-Output ("  {0,-7} swept -1..+1 on a saturated pixel: folds={1}" -f $mode, $folds)
}

Write-Output '== 3. WHAT EACH DIAL DOES (grey, output code) =='
Write-Output ("  {0,-5} {1,6} {2,6} {3,6} {4,6}" -f 'in','sh+5','sh-5','pu+5','pu-5')
# A grey pixel has R=G=B, and the Rec.709 weights sum to 1, so its luma IS the
# channel value. Calling Luma with one argument here silently zeroed G and B and
# scaled the answer by 0.2126 -- which is why the first version of this table read
# as if every dial crushed the image.
foreach ($c in @(16,32,64,96,128,160,192,240)) {
    $lin = Dec ($c/255.0)
    $a = Enc (Taste $lin $lin $lin  1  0).R
    $b = Enc (Taste $lin $lin $lin -1  0).R
    $e = Enc (Taste $lin $lin $lin  0  1).R
    $f = Enc (Taste $lin $lin $lin  0 -1).R
    Write-Output ("  {0,-5} {1,6} {2,6} {3,6} {4,6}" -f $c, [int]($a*255), [int]($b*255), [int]($e*255), [int]($f*255))
}
