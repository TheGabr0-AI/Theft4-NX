# Theft4-NX: one full generation of ultimate/app/generated from nothing (docs/switch-port/20, U3/U4):
# the folder is deleted first (no partial output reused), then the code generator, then tools/post_codegen.py in full,
# then tools/ultimate/generation_manifest.py. Each step's end is checked; the run stops at the first failure.
#
#   powershell -ExecutionPolicy Bypass -File tools/ultimate/regenerate.ps1 -Manifest <out.json> [-MinFreeGB 6]
#
# Needs THEFT4_PYTHON (a Windows python.exe with xxhash). The passes' switches are the ones tools/ultimate/env.bat sets.
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [double]$MinFreeGB = 6
)
$ErrorActionPreference = "Stop"
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Ult = Join-Path $Repo "ultimate"
$Gen = Join-Path $Ult "app\generated"
$Logs = Join-Path $Ult "out\regen"
if (-not $env:THEFT4_PYTHON) { throw "set THEFT4_PYTHON" }

$os = Get-CimInstance Win32_OperatingSystem
$free = $os.FreePhysicalMemory / 1MB
if ($free -lt $MinFreeGB) { throw ("only {0:N1} GB of RAM free (less than {1} GB): not starting" -f $free, $MinFreeGB) }
Write-Host ("regenerate: {0:N1} GB RAM free" -f $free)

# The same switches as tools/ultimate/env.bat (GTA IV's calling-convention census).
$env:SASRT_REG_LIVENESS_EXIT_ABI = "0"
$env:REXSDK_DIR = Join-Path $Ult "sdk"
$env:REXGLUE_DUMP_IMAGE = Join-Path $Ult "out\renut_image.bin"

if (Test-Path $Logs) { Remove-Item -Recurse -Force $Logs }
New-Item -ItemType Directory -Force $Logs | Out-Null
# The switches the passes read, for the manifest (generation_manifest.py reads this file, not its own environment).
Get-ChildItem env: | Where-Object { $_.Name -match '^(SASRT_|BANJO_|BLUR_|REX_)' } | Sort-Object Name |
    ForEach-Object { "$($_.Name)=$($_.Value)" } | Set-Content -Encoding ascii (Join-Path $Logs "environment.txt")
if (Test-Path $Gen) { Remove-Item -Recurse -Force $Gen }
if (Test-Path $Gen) { throw "could not delete $Gen" }
Write-Host "regenerate: generated/ deleted"

$t0 = Get-Date
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Ult "tools\codegen.ps1") -Log (Join-Path $Logs "codegen.log") | Out-Null
$code = $LASTEXITCODE
$summary = Select-String -Path (Join-Path $Logs "codegen.log") -Pattern "Codegen summary:" | Select-Object -Last 1
if ($code -ne 0 -or -not $summary) { throw "codegen failed (exit $code)" }
Write-Host ("regenerate: codegen done in {0:N0} s: {1}" -f ((Get-Date) - $t0).TotalSeconds, $summary.Line.Trim())

$t1 = Get-Date
Push-Location $Ult
try {
    $ErrorActionPreference = "Continue"
    & $env:THEFT4_PYTHON tools/post_codegen.py *> (Join-Path $Logs "post_codegen.log")
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
} finally { Pop-Location }
if ($code -ne 0) { throw "post_codegen.py failed (exit $code)" }
if (-not (Select-String -Path (Join-Path $Logs "post_codegen.log") -Pattern "optimised codegen steps done" -Quiet)) {
    throw "post_codegen.py did not reach its last step"
}
Write-Host ("regenerate: post_codegen done in {0:N0} s" -f ((Get-Date) - $t1).TotalSeconds)

& $env:THEFT4_PYTHON (Join-Path $PSScriptRoot "generation_manifest.py") --ultimate $Ult --logs $Logs --out $Manifest
if ($LASTEXITCODE -ne 0) { throw "generation_manifest.py failed" }
Write-Host "regenerate: done"
