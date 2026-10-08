# taste_check.ps1 -- mirror ps_ml_taste.hlsl and assert the properties it must have,
# plus the reason exposure trim is NOT in this stage.
#
# The regression this exists to catch, twice over:
#  (a) the shader's first version decoded with code^2 and re-encoded with ^(2.2)
#      instead of ^(1/2.2), compounding the frame to ~code^4.4 -- ANY non-zero dial
#      crushed the picture while all-zero passed through and looked fine. Identity at
#      neutral is therefore check #1, not a nicety.
#  (b) trim moved to this stage and blew saturated highlights, because a
#      display-referred Hable has already promised its top end to 1.0. Section 5
#      measures that instead of arguing about it.
#
# Returns pscustomobject, never arrays: array indexing inside functions has twice
# produced op_Multiply failures in this harness that had nothing to do with the maths.
$ErrorActionPreference = 'Stop'
$K = 48.0 / 255.0

function Taste([double]$cr,[double]$cg,[double]$cb,[double]$lift,[double]$punch) {
    $lr = [math]::Pow($cr, 2.2); $lg = [math]::Pow($cg, 2.2); $lb = [math]::Pow($cb, 2.2)
    $qr = [math]::Max($lr, 1e-6); $qg = [math]::Max($lg, 1e-6); $qb = [math]::Max($lb, 1e-6)

    $yIn = 0.2126*$qr + 0.7152*$qg + 0.0722*$qb
    $y   = [math]::Pow($yIn, 1.0/2.2)
    $q   = [math]::Pow(2.0, $lift * 0.6)
    if ($y -lt $K) { $y = $K * (1.0 - [math]::Pow(1.0 - $y/$K, $q)) }
    $s   = [math]::Pow(2.0, -$punch)
    $d   = [math]::Abs($y - 0.5)
    $sgn = 0.0; if ($y -gt 0.5) { $sgn = 1.0 } elseif ($y -lt 0.5) { $sgn = -1.0 }
    $y   = [math]::Min(1.0, [math]::Max(0.0, 0.5 + $sgn * 0.5 * [math]::Pow([math]::Max(2.0*$d,0.0), $s)))
    $yOut = [math]::Max([math]::Pow($y, 2.2), 1e-6)

    $scale = $yOut / $yIn
    $hi2 = [math]::Max($qr, [math]::Max($qg, $qb))
    if ($scale -gt 1.0) { $scale = [math]::Min($scale, 1.0/$hi2) }

    return [pscustomobject]@{
        R = [math]::Pow([math]::Min(1.0, $qr*$scale), 1/2.2)
        G = [math]::Pow([math]::Min(1.0, $qg*$scale), 1/2.2)
        B = [math]::Pow([math]::Min(1.0, $qb*$scale), 1/2.2)
    }
}
function LumaOf($o) { 0.2126*$o.R + 0.7152*$o.G + 0.0722*$o.B }

$cols = @(
    [pscustomobject]@{n='near black'; r=0.02; g=0.05; b=0.03},
    [pscustomobject]@{n='deep blue';  r=0.10; g=0.15; b=0.60},
    [pscustomobject]@{n='sky';        r=0.35; g=0.55; b=0.90},
    [pscustomobject]@{n='mid grey';   r=0.50; g=0.50; b=0.50},
    [pscustomobject]@{n='skin';       r=0.65; g=0.45; b=0.35},
    [pscustomobject]@{n='red face';   r=0.90; g=0.30; b=0.30},
    [pscustomobject]@{n='near white'; r=0.99; g=0.98; b=0.97}
)

# 1. IDENTITY at neutral ------------------------------------------------------
$worst = 0.0
foreach ($c in $cols) {
    $o = Taste $c.r $c.g $c.b 0.0 0.0
    $worst = [math]::Max($worst, [math]::Abs($o.R-$c.r))
    $worst = [math]::Max($worst, [math]::Abs($o.G-$c.g))
    $worst = [math]::Max($worst, [math]::Abs($o.B-$c.b))
}
Write-Output ("1. identity at neutral dials : worst channel error {0:E2}   {1}" -f $worst,
    $(if ($worst -lt 1e-6){'PASS'}else{'FAIL -- neutral is not a no-op'}))

# 2. hue / saturation preserved -----------------------------------------------
$drift = 0.0
foreach ($c in $cols) {
    $sIn = $c.r + $c.g + $c.b
    foreach ($t in @(-1.0,-0.4,0.4,1.0)) { foreach ($p in @(-1.0,-0.4,0.4,1.0)) {
        $o = Taste $c.r $c.g $c.b $t $p
        $sO = $o.R + $o.G + $o.B
        if ($sO -le 0) { continue }
        $drift = [math]::Max($drift, [math]::Abs($c.r/$sIn - $o.R/$sO))
        $drift = [math]::Max($drift, [math]::Abs($c.g/$sIn - $o.G/$sO))
        $drift = [math]::Max($drift, [math]::Abs($c.b/$sIn - $o.B/$sO))
    }}
}
Write-Output ("2. channel-ratio drift       : {0:E2}            {1}" -f $drift,
    $(if ($drift -lt 1e-9){'PASS (colour untouched)'}else{'FAIL -- colour moves with the dials'}))

# 3. monotonicity, per-dial direction ----------------------------------------
# shadow must RAISE luma as the dial rises. punch is a contrast curve about mid grey,
# so it must LOWER a below-mid pixel and RAISE an above-mid one. A single "never
# decreases" rule flags every correct punch step -- which is what the first version of
# this check did, and the FAIL was in the check.
foreach ($probe in @(@{n='below mid'; r=0.25; g=0.45; b=0.30}, @{n='above mid'; r=0.70; g=0.80; b=0.75})) {
    foreach ($mode in @('shadow','punch')) {
        $prev = -1.0; $folds = 0
        for ($i=0; $i -le 255; $i++) {
            $dial = ($i/255.0)*2.0 - 1.0
            $li = 0.0; $pu = 0.0
            if ($mode -eq 'shadow') { $li = $dial } else { $pu = $dial }
            $y = LumaOf (Taste $probe.r $probe.g $probe.b $li $pu)
            $bad = $false
            if ($prev -ge 0.0) {
                if ($mode -eq 'punch') {
                    if ($probe.n -eq 'below mid') { $bad = ($y -gt $prev + 1e-9) } else { $bad = ($y -lt $prev - 1e-9) }
                } else { $bad = ($y -lt $prev - 1e-9) }
            }
            if ($bad) { $folds++ }
            $prev = $y
        }
        Write-Output ("3. {0,-7} {1,-10}   : folds={2}   {3}" -f $mode, $probe.n, $folds,
            $(if($folds -eq 0){'PASS'}else{'FAIL'}))
    }
}
$piv = Taste 0.5 0.5 0.5 0.0 1.0
$pivErr = [math]::Max([math]::Abs($piv.R-0.5), [math]::Max([math]::Abs($piv.G-0.5), [math]::Abs($piv.B-0.5)))
Write-Output ("3b. mid-grey pivot, full punch : drift {0:E2}   {1}" -f $pivErr,
    $(if ($pivErr -lt 1e-9){'PASS (pivot immovable)'}else{'FAIL'}))

# 4. what each dial delivers (grey ramp, output codes) -----------------------
Write-Output ''
Write-Output ("4. {0,-5} {1,6} {2,6} {3,6} {4,6}" -f 'in','sh+5','sh-5','pu+5','pu-5')
foreach ($c in @(16,32,48,64,96,128,160,192,240)) {
    $v = $c/255.0
    Write-Output ("  {0,-5} {1,6} {2,6} {3,6} {4,6}" -f $c,
        [int]((LumaOf (Taste $v $v $v  1.0  0.0))*255),
        [int]((LumaOf (Taste $v $v $v -1.0  0.0))*255),
        [int]((LumaOf (Taste $v $v $v  0.0  1.0))*255),
        [int]((LumaOf (Taste $v $v $v  0.0 -1.0))*255))
}
Write-Output ("   (shadow must be identity at and above code {0})" -f [int]($K*255))

# 5. WHY TRIM IS PRE-CURVE ---------------------------------------------------
# The Unplugged branch is display-referred Hable -- hable(x)/hable(4.8), asymptoting
# below 1.0 -- so a gain BEFORE it is absorbed by the shoulder. The same gain AFTER it
# can only push channels past full scale. Measured, not argued: this is the blown
# highlights the user reported when trim moved post-curve.
function Hable1([double]$x) {
    $A=0.15; $B=0.50; $C=0.10; $D=0.20; $E=0.02; $F=0.30
    return (($x*($A*$x+$C*$B)+$D*$E) / ($x*($A*$x+$B)+$D*$F)) - ($E/$F)
}
$den = Hable1 4.8
Write-Output ''
# The ramp must span the REAL signal range, not a pretty one. After
# ST2084ToLinear(x, 10000/sdrNits), 1.0 == sdrNits, so a 1000-nit master on a
# 127-nit target reaches x ~ 7.9. The first version of this test ramped 0..1.2 and
# reported zero clipping for both placements -- a clean answer to the wrong question.
$XMAX = 1000.0 / 127.0
Write-Output ("5. trim placement, same gain, input ramp 0..{0:F1} (1.0 = sdrNits=127):" -f $XMAX)
Write-Output '   trim    pre-curve clipped   post-curve clipped   post mean out'
foreach ($ts in @(1,2,3,5)) {
    $g = [math]::Pow(2.0, $ts/3.0)
    $cp = 0; $cq = 0; $sum = 0.0
    for ($i=0; $i -le 255; $i++) {
        $x = ($i/255.0) * $XMAX
        if (((Hable1 ($x*$g) / $den) -gt 1.0) -or ((Hable1 ($x*$g) / $den) -lt 0.0)) { $cp++ }
        $post = (Hable1 $x / $den) * $g
        if ($post -gt 1.0) { $cq++ }
        $sum += [math]::Min($post, 1.0)
    }
    Write-Output ("   +{0} ({1,4:F2}x)   {2,8} of 256   {3,8} of 256        {4:F3}" -f $ts, $g, $cp, $cq, ($sum/256.0))
}
Write-Output '   (pre-curve the shoulder absorbs the gain; post-curve it can only clip)'
