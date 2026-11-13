@echo off
rem Theft4-NX: the Ultimate Base's PC build for GTA IV (ultimate\tools\build_pc.bat with this project's paths), into
rem ultimate\app\out\pc. Renut's renderer is built in (BANJO_RENUT_RENDERER=ON) with the hooks bound to GTA IV's
rem functions (docs/switch-port/20, U4b). Environment, besides tools\ultimate\env.bat's THEFT4_VS_ROOT / THEFT4_PYTHON:
rem   THEFT4_SHADER_DUMP  the folder of GTA IV's 1,370 extracted shader containers (<HASH>.<vs|ps>.bin; game data,
rem                       local, never committed; read only)
rem   THEFT4_BUILD_JOBS   parallel jobs (default 4: the generated files take about 1 GB each to compile)
rem Rule (docs/switch-port/09-rules.md section 7): no other build or regeneration running.
setlocal
call "%~dp0env.bat" || exit /b 1
if "%THEFT4_BUILD_JOBS%"=="" set "THEFT4_BUILD_JOBS=4"
if not defined THEFT4_SHADER_DUMP (
  echo build_pc.bat: set THEFT4_SHADER_DUMP to the folder of GTA IV's shader containers
  exit /b 1
)
set "BUILD_DIR=%THEFT4_ULTIMATE%\app\out\pc"
set "SHADER_DUMP=%THEFT4_ULTIMATE:\=/%/out/renut_shader_dump"
rem The base reads the game's shader containers from ultimate\out\renut_shader_dump (cmake\banjo_renut_gpu.cmake sets it as
rem a normal variable, over gpu.cmake's cache entry), named as the base names them (<vs|ps>_<xxh3>.bin):
rem tools\ultimate\gta4_shader_dump.py copies THEFT4_SHADER_DUMP's containers there and checks each (U6; until then a
rem junction to THEFT4_SHADER_DUMP, whose names hid the pixel shaders from tools\renut_early_alpha.py and let the
rem capture step copy duplicates into THEFT4_SHADER_DUMP). The folder must exist before configure: whether it does
rem decides at configure time if tools\dump_shaders.py (Katamari's dump of default.xex) would run.
"%THEFT4_PYTHON%" -I "%~dp0gta4_shader_dump.py" --containers "%THEFT4_SHADER_DUMP%" --out "%THEFT4_ULTIMATE%\out\renut_shader_dump" || exit /b 1
(
  cmake -S "%THEFT4_ULTIMATE%\app" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON "-DREXSDK_DIR=%THEFT4_ULTIMATE%\sdk" -DBANJO_RENUT_RENDERER=ON "-DRENUT_SHADER_DUMP_DIR=%SHADER_DUMP%" || exit /b 1
)
cmake --build "%BUILD_DIR%" -j %THEFT4_BUILD_JOBS% %* || exit /b 1
echo build_pc.bat: built in %BUILD_DIR%
