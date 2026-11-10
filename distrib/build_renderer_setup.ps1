# build_renderer_setup.ps1 -- compile distrib\UnpluggedRenderer.iss into a
# standalone setup.exe for the Unplugged video renderer.
#
# Gates before ISCC runs, because each failure mode is silent otherwise:
#   1. Inno Setup 6 exists.
#   2. versions agree: Include\Version.h + revision.h vs the #define in the
#      .iss. A stale version in the installer name is how a user ends up
#      installing yesterday's filter.
#   3. make_dist.cmd refreshes the staging folder from _bin\Filter_x64, so
#      the payload is the binary that was actually built, not a folder left
#      over from an older build.
#   4. every Source path exists. Inno fails on a missing Source, but a wrong
#      Source that happens to exist (an old filter folder) compiles fine and
#      ships the wrong bytes.
#   5. the VC++ redist is present, is the same one the shared vc_runtime.iss
#      checks for, and is not older than the required version.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File distrib\build_renderer_setup.ps1
#   ... -Fast     # no compression, output named *-TESTONLY.exe

param(
    [switch]$Fast
)

$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$root = Split-Path $here -Parent
$iss  = Join-Path $here 'UnpluggedRenderer.iss'

function Fail([string]$msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# 1. compiler -----------------------------------------------------------------
$iscc = $null
foreach ($p in @(
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $p) { $iscc = $p; break }
}
if (-not $iscc) {
    Fail "Inno Setup 6 not found. Install it (winget install JRSoftware.InnoSetup) and re-run."
}
Write-Host "ISCC: $iscc"

# 2. version agreement --------------------------------------------------------
$versionH  = Get-Content (Join-Path $root 'Include\Version.h') -Raw
$revisionH = Get-Content (Join-Path $root 'revision.h') -Raw
function Read-Def([string]$text, [string]$name) {
    $m = [regex]::Match($text, "#define\s+$name\s+(\d+)")
    if (-not $m.Success) { return $null }
    return [int]$m.Groups[1].Value
}
$maj = Read-Def $versionH 'VER_MAJOR'
$min = Read-Def $versionH 'VER_MINOR'
$bld = Read-Def $versionH 'VER_BUILD'
$rev = Read-Def $revisionH 'REV_NUM'
if ($null -eq $maj -or $null -eq $min -or $null -eq $bld -or $null -eq $rev) {
    Fail "could not read VER_MAJOR/VER_MINOR/VER_BUILD from Include\Version.h or REV_NUM from revision.h"
}
$version = "$maj.$min.$bld.$rev"
Write-Host "renderer build: $version"

$issText = Get-Content $iss -Raw
$rb = [regex]::Match($issText, '#define\s+RendererBuild\s+"([^"]+)"')
if (-not $rb.Success) { Fail "RendererBuild not found in UnpluggedRenderer.iss" }
if ($rb.Groups[1].Value -ne $version) {
    $msg = "UnpluggedRenderer.iss stages renderer build {0} but the tree is {1}. " -f $rb.Groups[1].Value, $version
    $msg += "Update RendererBuild in the .iss, or rebuild the renderer."
    Fail $msg
}

# The package version follows the Unplugged application release, not the
# filter build: one release, one version for users to quote in a report.
$vm = [regex]::Match($issText, '#define\s+MyAppVersion\s+"([^"]+)"')
if (-not $vm.Success) { Fail "MyAppVersion not found in UnpluggedRenderer.iss" }
$appRoot = Join-Path (Split-Path $root -Parent) 'unplugged-mpc-hc'
$appIss  = Join-Path $appRoot 'distrib\unplugged.iss'
if (-not (Test-Path $appIss)) { Fail "missing $appIss (the application release version lives there)" }
$av = [regex]::Match((Get-Content $appIss -Raw), '#define\s+MyAppVersion\s+"([^"]+)"')
if (-not $av.Success) { Fail "MyAppVersion not found in $appIss" }
if ($vm.Groups[1].Value -ne $av.Groups[1].Value) {
    $msg = "renderer setup says package version {0} but the Unplugged application is {1}. " -f $vm.Groups[1].Value, $av.Groups[1].Value
    $msg += "Set MyAppVersion in UnpluggedRenderer.iss to match the application release."
    Fail $msg
}
Write-Host "package version: $($vm.Groups[1].Value) (matches Unplugged)"

$stageName = "UnpluggedRenderer-$version"
$stageDir  = Join-Path $root "_bin\$stageName"

# 3. refresh the staging folder ----------------------------------------------
Write-Host "staging payload with make_dist.cmd..."
& cmd.exe /c "`"$(Join-Path $here 'make_dist.cmd')`""
if ($LASTEXITCODE -ne 0) {
    Fail "make_dist.cmd failed ($LASTEXITCODE). Build the renderer Release x64 first."
}

# 4. payload gates ------------------------------------------------------------
$payload = @(
    'unplugged_vr.ax',
    'onnxruntime.dll',
    'xgb_gamma_model.plxgb',
    'xgb_highlight_rolloff.plxgb',
    'xgb_shadow_recovery.plxgb',
    'yolo26n-seg_op21.onnx',
    'yolo26n-sem.onnx'
)
foreach ($f in $payload) {
    $p = Join-Path $stageDir $f
    if (-not (Test-Path $p)) { Fail "missing payload file: $p" }
}

$license = Join-Path $root 'LICENSE.txt'
if (-not (Test-Path $license)) { Fail "missing $license (the GPL text is the LicenseFile)" }
$readme = Join-Path $here 'Readme-UnpluggedRenderer.txt'
if (-not (Test-Path $readme)) { Fail "missing $readme" }
$reset = Join-Path $here 'Reset_Settings.cmd'
if (-not (Test-Path $reset)) { Fail "missing $reset" }

# 5. VC++ runtime -------------------------------------------------------------
$vcIss = Join-Path $appRoot 'distrib\vc_runtime.iss'
$redist = Join-Path $appRoot 'distrib\redist\vc_redist.x64.exe'
if (-not (Test-Path $vcIss))  { Fail "missing $vcIss (shared VC runtime check)" }
if (-not (Test-Path $redist)) { Fail "missing $redist" }

$vcText = Get-Content $vcIss -Raw
$reqMaj = Read-Def $vcText 'VC_REQ_MAJ'
$reqMin = Read-Def $vcText 'VC_REQ_MIN'
$reqBld = Read-Def $vcText 'VC_REQ_BLD'
if ($null -eq $reqMaj -or $null -eq $reqMin -or $null -eq $reqBld) {
    Fail "could not read VC_REQ_* from vc_runtime.iss"
}
$redistVer = (Get-Item $redist).VersionInfo.FileVersion
$rp = $redistVer.Split('.')
if ($rp.Count -lt 3) { Fail "cannot read the bundled redist version: $redistVer" }
$rMaj = [int]$rp[0]; $rMin = [int]$rp[1]; $rBld = [int]$rp[2]
$redistOk = ($rMaj -gt $reqMaj) -or
            ($rMaj -eq $reqMaj -and $rMin -gt $reqMin) -or
            ($rMaj -eq $reqMaj -and $rMin -eq $reqMin -and $rBld -ge $reqBld)
if (-not $redistOk) {
    Fail ("bundled vc_redist.x64.exe is $redistVer but vc_runtime.iss requires " +
          "$reqMaj.$reqMin.$reqBld or newer")
}
Write-Host "VC++ redist: $redistVer (required $reqMaj.$reqMin.$reqBld)"

# compile ---------------------------------------------------------------------
$isccArgs = @()
if ($Fast) { $isccArgs += '/DFastTest=1' }
$isccArgs += $iss
Write-Host "compiling..."
& $iscc @isccArgs
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed: $LASTEXITCODE" }

$out = Join-Path $here 'out'
Write-Host ""
Write-Host "setup built: $out" -ForegroundColor Green
Get-ChildItem $out -Filter 'UnpluggedRenderer-setup-*.exe' |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 |
    ForEach-Object {
        Write-Host ("  {0}  ({1:N0} bytes)  {2}" -f $_.Name, $_.Length, $_.LastWriteTime)
    }
if ($Fast) {
    Write-Host "NOTE: -Fast build, uncompressed, named TESTONLY. Not for shipping." -ForegroundColor Yellow
}