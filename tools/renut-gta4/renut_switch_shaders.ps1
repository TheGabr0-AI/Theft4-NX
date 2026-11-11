# The game-shader inputs of the Switch build of ReNut's renderer (docs/switch-port/18-renut-migration.md, step R5),
# made on the PC from the PC build's translator, as sa2-nx makes its own (tools/deko3d_*.py of banjo-nx / sa2-nx):
#
#   1. Renut's XenosRecomp (the PC build's, out/build/pc-eu-base/xenos_recomp) over GTA IV's 1,370 extracted
#      containers with BANJO_DEKO3D_SPIRV_DIR: one <vs|ps>_<hash>.spv each (-DBANJO_DEKO3D, D3D clip space). Its
#      shader cache is written to scratch and must equal the PC build's (the same translator and inputs).
#   2. tools/deko3d/deko3d_shaders.py: SPIRV-Cross (Vulkan GLSL 4.60), deko3d_glsl.py, uam for the default variant.
#   3. tools/deko3d/deko3d_glsl_opt.py --check: banjo-nx's GLSL passes for uam (a shader that fails keeps its text).
#   4. tools/deko3d/deko3d_shader_package.py: deko3d_glsl.bin + deko3d_glsl_switch.cpp (embedded with .incbin).
#   5. tools/renut-gta4/renut_switch_shader_cache.py: the PC build's shader table with no DXIL and no SPIR-V.
#
# Output: out/renut-switch (game data: never committed, never distributed apart from a private test NRO).
#   powershell -File tools/renut-gta4/renut_switch_shaders.ps1 [-Containers <dir>] [-Python <python.exe>]
param(
    [string]$Containers = "C:/dev/Theft4/eu-private/offline-shaders/containers",
    [string]$Python = "D:/Python/python.exe"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$out = Join-Path $root "out/renut-switch"
$recomp = Join-Path $root "out/build/pc-eu-base/xenos_recomp/build/XenosRecomp/XenosRecomp.exe"
$pcCache = Join-Path $root "out/build/pc-eu-base/renut_gpu/shader_cache.cpp"
foreach ($need in @($recomp, $pcCache, $Containers)) {
    if (-not (Test-Path $need)) { throw "renut_switch_shaders: $need missing (build tools/pc-eu-base with THEFT4_RENUT_ENGINE first)" }
}
New-Item -ItemType Directory -Force "$out/spirv", "$out/scratch" | Out-Null
Get-ChildItem "$out/spirv" -File | Remove-Item -Force

Write-Host "== 1. XenosRecomp -> deko3d SPIR-V"
$env:PATH = (Join-Path $root "tools/XenosRecomp/thirdparty/dxc-bin/bin/x64") + ";" + $env:PATH
$env:BANJO_DEKO3D_SPIRV_DIR = "$out/spirv"
& $recomp $Containers "$out/scratch/shader_cache_check.cpp" (Join-Path $root "thirdparty/renut/reblue-XenosRecomp/XenosRecomp/shader_common.h") *> "$out/xenosrecomp.log"
if ($LASTEXITCODE -ne 0) { throw "XenosRecomp failed ($LASTEXITCODE): $out/xenosrecomp.log" }
Remove-Item Env:BANJO_DEKO3D_SPIRV_DIR
$spv = (Get-ChildItem "$out/spirv" -Filter *.spv).Count
$fail = (Get-ChildItem "$out/spirv" -Filter *.fail).Count
Write-Host "   $spv SPIR-V, $fail failed"
if ((Get-FileHash "$out/scratch/shader_cache_check.cpp").Hash -ne (Get-FileHash $pcCache).Hash) {
    throw "the translator's cache differs from the PC build's ($pcCache): rebuild the PC tree first"
}

Write-Host "== 2. SPIRV-Cross, deko3d_glsl.py, uam"
& $Python -I (Join-Path $root "tools/deko3d/deko3d_shaders.py") --spirv "$out/spirv" --out "$out/deko3d" --jobs 6
if ($LASTEXITCODE -ne 0) { throw "deko3d_shaders.py failed" }

Write-Host "== 3. GLSL passes"
& $Python -I (Join-Path $root "tools/deko3d/deko3d_glsl_opt.py") --in "$out/deko3d/glsl" --out "$out/deko3d/glsl_opt" --check --jobs 6
if ($LASTEXITCODE -ne 0) { throw "deko3d_glsl_opt.py failed" }

Write-Host "== 4. GLSL package"
& $Python -I (Join-Path $root "tools/deko3d/deko3d_shader_package.py") --glsl "$out/deko3d/glsl_opt" --out $out
if ($LASTEXITCODE -ne 0) { throw "deko3d_shader_package.py failed" }

Write-Host "== 5. shader table"
& $Python -I (Join-Path $root "tools/renut-gta4/renut_switch_shader_cache.py") --out $out
if ($LASTEXITCODE -ne 0) { throw "renut_switch_shader_cache.py failed" }
