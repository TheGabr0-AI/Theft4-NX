# Puts the Ultimate Base SDK's third-party libraries (sdk/thirdparty: FFmpeg, simde, SDL3, glslang, Vulkan, tracy, ...)
# into ultimate/sdk/thirdparty, taken from the same pinned commit as the rest of ultimate/ (docs/switch-port/20 §3).
# They are not committed here (372 MB); their licences are the ones in that commit.
#
#   powershell -ExecutionPolicy Bypass -File tools/ultimate/fetch_sdk_thirdparty.ps1 [-BaseClone <dir>] [-Force]
#
# -BaseClone: a clone of https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base that has the pinned commit
#             (default out/ultimate-base-src, cloned there when missing).
param(
    [string]$BaseClone = "",
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$Commit = Get-Content (Join-Path $PSScriptRoot "BASE_COMMIT") -TotalCount 1
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $BaseClone) { $BaseClone = Join-Path $Repo "out\ultimate-base-src" }
$Dest = Join-Path $Repo "ultimate\sdk\thirdparty"

if ((Test-Path $Dest) -and -not $Force) {
    Write-Host "fetch_sdk_thirdparty: $Dest exists (use -Force to replace it)"
    exit 0
}
if (-not (Test-Path (Join-Path $BaseClone ".git"))) {
    git clone https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base.git $BaseClone
    if ($LASTEXITCODE -ne 0) { throw "clone failed" }
}
# (Windows PowerShell drops the braces of "^{commit}" on the way to git: ask for the object's type instead.)
function Test-BaseCommit {
    $ErrorActionPreference = "Continue"
    $type = git -C $BaseClone cat-file -t $Commit 2>$null
    return ($LASTEXITCODE -eq 0 -and "$type".Trim() -eq "commit")
}
if (-not (Test-BaseCommit)) {
    git -C $BaseClone fetch origin
    if (-not (Test-BaseCommit)) { throw "commit $Commit not in $BaseClone" }
}
$Tar = Join-Path ([IO.Path]::GetTempPath()) "theft4_ultimate_sdk_thirdparty.tar"
git -C $BaseClone archive --format=tar --prefix=ultimate/sdk/ -o $Tar "${Commit}:sdk" -- thirdparty
if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
# Windows' own tar: MSYS's tar reads C: as a remote host.
& "$env:SystemRoot\System32\tar.exe" -xf $Tar -C $Repo
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Remove-Item $Tar
# FFmpeg's generated headers, which FFmpeg's own .gitignore kept out of the base's commit (tools/ultimate/ffmpeg/config.h
# says why): without them the SDK stops at the first libavutil file.
$FF = Join-Path $PSScriptRoot "ffmpeg"
Copy-Item (Join-Path $FF "config.h") (Join-Path $Dest "FFmpeg\config.h") -Force
Copy-Item (Join-Path $FF "libavutil\avconfig.h") (Join-Path $Dest "FFmpeg\libavutil\avconfig.h") -Force
Copy-Item (Join-Path $FF "libavutil\ffversion.h") (Join-Path $Dest "FFmpeg\libavutil\ffversion.h") -Force
$n = (Get-ChildItem -Recurse -File $Dest).Count
Write-Host "fetch_sdk_thirdparty: $n files from $Commit in $Dest"
