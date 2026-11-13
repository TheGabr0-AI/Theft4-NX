@echo off
rem Theft4-NX: the Ultimate Base's PC build for GTA IV (ultimate\tools\build_pc.bat with this project's paths), into
rem ultimate\app\out\pc. Renut's renderer is built in (BANJO_RENUT_RENDERER=ON) with the hooks bound to GTA IV's
rem functions (docs/switch-port/20, U4b). Environment, besides tools\ultimate\env.bat's THEFT4_VS_ROOT / THEFT4_PYTHON:
rem   THEFT4_SHADER_DUMP  the folder of GTA IV's shader containers (*.bin; game data, local, never committed)
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
set "SHADER_DUMP=%THEFT4_SHADER_DUMP:\=/%"
rem The base reads the game's shader containers from ultimate\out\renut_shader_dump (cmake\banjo_renut_gpu.cmake sets it as
rem a normal variable, over gpu.cmake's cache entry): a junction to THEFT4_SHADER_DUMP. Configure runs every time: whether
rem that folder exists decides at configure time if tools\dump_shaders.py (Katamari's dump of default.xex) would run.
if not exist "%THEFT4_ULTIMATE%\out\renut_shader_dump" (
  if not exist "%THEFT4_ULTIMATE%\out" mkdir "%THEFT4_ULTIMATE%\out"
  mklink /J "%THEFT4_ULTIMATE%\out\renut_shader_dump" "%THEFT4_SHADER_DUMP%" >nul || exit /b 1
)
rem A failed earlier configure can leave an empty folder there: the shader cache then comes out empty (no error).
if not exist "%THEFT4_ULTIMATE%\out\renut_shader_dump\*.bin" (
  echo build_pc.bat: no shader containers in %THEFT4_ULTIMATE%\out\renut_shader_dump: remove it if empty and run again
  exit /b 1
)
(
  cmake -S "%THEFT4_ULTIMATE%\app" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON "-DREXSDK_DIR=%THEFT4_ULTIMATE%\sdk" -DBANJO_RENUT_RENDERER=ON "-DRENUT_SHADER_DUMP_DIR=%SHADER_DUMP%" || exit /b 1
)
cmake --build "%BUILD_DIR%" -j %THEFT4_BUILD_JOBS% %* || exit /b 1
echo build_pc.bat: built in %BUILD_DIR%
