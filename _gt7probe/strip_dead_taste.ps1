# strip_dead_taste.ps1 -- remove the superseded taste code from the converter shader.
# Marker-anchored and self-verifying on purpose: a previous line-surgery attempt in
# this tree silently dropped a quote and the BOM, so this asserts every boundary and
# refuses to write if any check fails, and preserves the file's exact encoding.
$ErrorActionPreference = 'Stop'
$path = 'C:\code\unplugged-VideoRenderer\Shaders\d3d11\ps_convert_pq_to_sdr.hlsl'
$bytes = [System.IO.File]::ReadAllBytes($path)
$hadBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hadBom) { $text = $text.TrimStart([char]0xFEFF) }
$nl = if ($text -match "`r`n") { "`r`n" } else { "`n" }
$L = New-Object System.Collections.Generic.List[string]
$L.AddRange([string[]]($text -split "`r?`n"))

# --- boundaries -------------------------------------------------------------
$a = -1
for ($i = 0; $i -lt $L.Count; $i++) {
    if ($L[$i] -match 'TASTE OPERATORS: three properties') { $a = $i - 1; break }   # the divider line
}
$b = -1
for ($i = 0; $i -lt $L.Count; $i++) { if ($L[$i] -match '^float4 main\(PS_INPUT input\)') { $b = $i; break } }

if ($a -lt 0) { throw 'start marker not found' }
if ($b -lt 0) { throw 'main() not found' }
if ($a -ge $b) { throw "start ($a) is not before main ($b)" }
if ($L[$a] -notmatch '^//\s*-{10,}\s*$') { throw "expected a divider at $a, got: [$($L[$a])]" }
# The line before main() may be blank; the last NON-BLANK line must be the closing
# brace of the function being removed.
$lastCode = $b - 1
while ($lastCode -gt $a -and $L[$lastCode].Trim() -eq '') { $lastCode-- }
if ($L[$lastCode].Trim() -ne '}') { throw "expected the block to end with }, found [$($L[$lastCode])] at line $($lastCode+1)" }
$b = $lastCode + 1   # remove through the closing brace, keep the blank separator
$region = ($L[$a..($b-1)] -join "`n")
foreach ($must in @('ApplyLook', 'kShadowKnee', 'kW709')) {
    if ($region -notmatch $must) { throw "region does not contain $must -- wrong boundaries" }
}
if ($region -match 'ST2084ToLinear|ToneMappingHable') { throw 'region contains live curve code -- aborting' }
Write-Output ("removing lines {0}..{1} ({2} lines); BOM was {3}, newline {4}" -f ($a+1), $b, ($b-$a), $hadBom, $(if ($nl -eq "`r`n") {'CRLF'} else {'LF'}))

$tombstone = @(
'// -----------------------------------------------------------------------------',
'// The taste operators (exposure trim, shadow detail, punch) used to live here and',
'// were MOVED to Shaders/d3d11/ps_ml_taste.hlsl, which draws after the automatic',
'// grade. They ran before it, so the model''s gamma, shadow slope and highlight',
'// rolloff were applied on top of the user''s dials and could undo them silently.',
'//',
'// expTrimGain / shadowLift / punch are still declared in the cbuffer below because',
'// the layout is shared with the C++ side, but this shader no longer reads them --',
'// see the Reference branch. One implementation of each shape, in one stage.',
'// -----------------------------------------------------------------------------'
)
$L.RemoveRange($a, $b - $a)
$L.InsertRange($a, [string[]]$tombstone)

$out = ($L -join $nl)
$enc = New-Object System.Text.UTF8Encoding($hadBom)   # preserve whatever the file had
[System.IO.File]::WriteAllText($path, $out, $enc)

# --- verify ------------------------------------------------------------------
$chk = [System.IO.File]::ReadAllText($path)
if ($chk -match 'float3 ApplyLook') { throw 'ApplyLook still present' }
if ($chk -notmatch 'taste operators .* used to live here') { throw 'tombstone missing' }
if ($chk -notmatch 'float4 main\(PS_INPUT input\)') { throw 'main() lost' }
$b2 = [System.IO.File]::ReadAllBytes($path)
Write-Output ("written: {0} bytes (was {1}); BOM {2}" -f $b2.Length, $bytes.Length, $(if ($hadBom) {'kept'} else {'none'}))
Write-Output 'OK'
