# taste_check.ps1 -- mirror ps_ml_taste.hlsl and assert the properties it must have.
# The regression this exists to catch: the shader's first version decoded with
# code^2 and re-encoded with ^(2.2) instead of ^(1/2.2), compounding the frame to
# code^4.4 -- so ANY non-zero dial crushed the picture while all-zero passed through
# and looked fine. Identity at neutral is therefore check #1, not a nicety.
#
# Returns pscustomobject, never arrays: array indexing inside functions has twice
# produced op_Multiply failures in this harness that had nothing to do with the maths.
$ErrorActionPreference = 'Stop'
$K = 48.0 / 255.0

function Taste([double]$cr,[double]$cg,[double]$cb,[double]$lift,[double]$punch,[double]$gain) {
    $lr = [math]::Pow($cr, 2.2); $lg = [math]::Pow($cg, 2.2); $lb = [math]::Pow($cb, 2.2)
    $tr = $lr * $gain; $tg = $lg * $gain; $tb = $lb * $gain
    $hi = [math]::Max($tr, [math]::Max($tg, $tb))
    if ($hi -gt 1.0) { $tr = $tr/$hi; $tg = $tg/$hi; $tb = $tb/$hi }
    $qr = [math]::Max($tr, 1e-6); $qg = [math]::Max($tg, 1e-6); $qb = [math]::Max($tb, 1e-6)

    $yIn  = 0.2126*$qr + 0.7152*$qg + 0.0722*$qb
    $y    = [math]::Pow($yIn, 1.0/2.2)
    $q    = [math]::Pow(2.0, $lift * 0.6)
    if ($y -lt $K) { $y = $K * (1.0 - [math]::Pow(1.0 - $y/$K, $q)) }
    $s    = [math]::Pow(2.0, -$punch)
    $d    = [math]::Abs($y - 0.5)
    $sgn  = 0.0; if ($y -gt 0.5) { $sgn = 1.0 } elseif ($y -lt 0.5) { $sgn = -1.0 }
    $y    = [math]::Min(1.0, [math]::Max(0.0, 0.5 + $sgn * 0.5 * [math]::Pow([math]::Max(2.0*$d,0.0), $s)))
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

# 1. IDENTITY at neutral -------------------------------------------------------
$worst = 0.0
foreach ($c in $cols) {
    $o = Taste $c.r $c.g $c.b 0.0 0.0 1.0
    $worst = [math]::Max($worst, [math]::Abs($o.R-$c.r))
    $worst = [math]::Max($worst, [math]::Abs($o.G-$c.g))
    $worst = [math]::Max($worst, [math]::Abs($o.B-$c.b))
}
$r1 = if ($worst -lt 1e-6) {'PASS'} else {'FAIL -- neutral is not a no-op'}
Write-Output ("1. identity at neutral dials : worst channel error {0:E2}   {1}" -f $worst, $r1)

# 2. hue / saturation preserved ------------------------------------------------
$drift = 0.0
foreach ($c in $cols) {
    $sIn = $c.r + $c.g + $c.b
    foreach ($t in @(-1.0,-0.4,0.4,1.0)) { foreach ($p in @(-1.0,-0.4,0.4,1.0)) {
        $o = Taste $c.r $c.g $c.b $t $p 1.0
        $sO = $o.R + $o.G + $o.B
        if ($sO -le 0) { continue }
        $drift = [math]::Max($drift, [math]::Abs($c.r/$sIn - $o.R/$sO))
        $drift = [math]::Max($drift, [math]::Abs($c.g/$sIn - $o.G/$sO))
        $drift = [math]::Max($drift, [math]::Abs($c.b/$sIn - $o.B/$sO))
    }}
}
$r2 = if ($drift -lt 1e-9) {'PASS (colour untouched)'} else {'FAIL -- colour moves with the dials'}
Write-Output ("2. channel-ratio drift       : {0:E2}            {1}" -f $drift, $r2)

# 3. nothing exceeds full scale ------------------------------------------------
$clip = 0
foreach ($ge in @(-5,-2,0,2,5)) {
    $gain = [math]::Pow(2.0, $ge/3.0)
    foreach ($c in $cols) {
        $o = Taste $c.r $c.g $c.b 0.0 0.0 $gain
        if ($o.R -gt 1.0001 -or $o.G -gt 1.0001 -or $o.B -gt 1.0001) { $clip++ }
    }
}
$r3 = if ($clip -eq 0) {'PASS'} else {'FAIL'}
Write-Output ("3. out-of-range outputs      : {0}                        {1}" -f $clip, $r3)

# 4. monotonicity -------------------------------------------------------------
# Direction is per-dial, and getting that wrong is a false alarm waiting to happen:
# shadow and trim must RAISE luma as the dial rises, but punch is a contrast curve
# about mid grey -- for a below-mid pixel it must LOWER luma, and for an above-mid
# pixel raise it. A single "never decreases" rule flags every correct punch step.
foreach ($probe in @(@{n='below mid'; r=0.25; g=0.45; b=0.30}, @{n='above mid'; r=0.70; g=0.80; b=0.75})) {
    foreach ($mode in @('shadow','punch','trim')) {
        $prev = -1.0; $folds = 0
        for ($i=0; $i -le 255; $i++) {
            $dial = ($i/255.0)*2.0 - 1.0
            $li = 0.0; $pu = 0.0; $ga = 1.0
            if ($mode -eq 'shadow') { $li = $dial } elseif ($mode -eq 'punch') { $pu = $dial }
            else { $ga = [math]::Pow(2.0, $dial*5.0/3.0) }
            $o = Taste $probe.r $probe.g $probe.b $li $pu $ga
            $y = LumaOf $o
            $bad = $false
            if ($prev -ge 0.0) {
                if ($mode -eq 'punch') {
                    # below mid: must not rise. above mid: must not fall.
                    if ($probe.n -eq 'below mid') { $bad = ($y -gt $prev + 1e-9) } else { $bad = ($y -lt $prev - 1e-9) }
                } else {
                    $bad = ($y -lt $prev - 1e-9)
                }
            }
            if ($bad) { $folds++ }
            $prev = $y
        }
        $r4 = if ($folds -eq 0) {'PASS'} else {'FAIL'}
        Write-Output ("4. {0,-7} {1,-10}    : folds={2}                        {3}" -f $mode, $probe.n, $folds, $r4)
    }
}

# The pivot must be immovable: punch is defined as contrast ABOUT mid grey.
$piv = Taste 0.5 0.5 0.5 0.0 1.0 1.0
$pivErr = [math]::Max([math]::Abs($piv.R-0.5), [math]::Max([math]::Abs($piv.G-0.5), [math]::Abs($piv.B-0.5)))
Write-Output ("4b. mid-grey pivot under full punch : drift {0:E2}   {1}" -f $pivErr, $(if ($pivErr -lt 1e-9){'PASS'}else{'FAIL'}))

# 5. delivered effect --------------------------------------------------------
Write-Output ''
Write-Output ("5. {0,-5} {1,6} {2,6} {3,6} {4,6} {5,6}" -f 'in','sh+5','sh-5','pu+5','pu-5','tr+5')
foreach ($c in @(16,32,64,96,128,160,192,240)) {
    $v = $c/255.0
    $a = LumaOf (Taste $v $v $v  1.0  0.0 1.0)
    $b = LumaOf (Taste $v $v $v -1.0  0.0 1.0)
    $e = LumaOf (Taste $v $v $v  0.0  1.0 1.0)
    $f = LumaOf (Taste $v $v $v  0.0 -1.0 1.0)
    $g = LumaOf (Taste $v $v $v  0.0  0.0 ([math]::Pow(2.0, 5.0/3.0)))
    Write-Output ("  {0,-5} {1,6} {2,6} {3,6} {4,6} {5,6}" -f $c,
        ([int]($a*255)), ([int]($b*255)), ([int]($e*255)), ([int]($f*255)), ([int]($g*255)))
}
