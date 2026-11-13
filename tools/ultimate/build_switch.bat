@echo off
rem Theft4-NX: the Ultimate Base's Switch NRO for GTA IV (ultimate\tools\build_switch.bat with this project's paths;
rem docs/switch-port/20, U7). deko3d only: no Vulkan driver linked (REXGLUE_SWITCH_NVK off, the SDK's default), so no
rem NVK SDK is needed. Usage:
rem   tools\ultimate\build_switch.bat [everyday|guard] [extra cmake --build args]
rem     everyday  ultimate\app\out\switch: the localized copies in place, no guard forms (the base's Switch default)
rem     guard     ultimate\app\out\switch-guard: the copies hooked with their guard forms (a guard or verify run,
rem               ultimate\tools\build_switch_guard.bat's options)
rem Environment, besides tools\ultimate\env.bat's THEFT4_VS_ROOT / THEFT4_PYTHON (CMake, Ninja and Python from there; the
rem Switch compiler is devkitA64's GCC through ultimate\tools\switch\cmake\switch-devkitA64.cmake):
rem   THEFT4_DEVKITPRO      devkitPro's folder (default C:/devkitPro: devkitA64, libnx with deko3d, portlibs zstd / z)
rem   THEFT4_SWITCH_JOBS    parallel jobs (default 2: GCC on the generated files needs far more memory than clang)
rem   THEFT4_NVK_SDK        a Mesa/NVK portlibs folder for plume's Vulkan headers (default <devkitPro>/portlibs/switch;
rem                         its libvulkan.a is not linked)
rem Inputs it checks before configuring (none is built here): ultimate\app\generated (tools\ultimate\regenerate.ps1),
rem ultimate\app\out\renut_switch (the shader package and table, ultimate\tools\switch_shaders.ps1), the PC tools for
rem the host shaders (tools\ultimate\build_deko3d_tools.bat) and glslangValidator (ultimate\tools\build_glslang.bat).
rem Every configure stamps the repository's commit into the NRO's log (THEFT4_BUILD_COMMIT, "-dirty" when ultimate\ or
rem tools\ultimate\ have uncommitted changes to tracked files).
rem Rule (docs/switch-port/09-rules.md section 7): no other build or regeneration running.
setlocal
rem This script's folder before any shift (shift moves %0 too).
set "HERE=%~dp0"
set "FLAVOUR=%~1"
if "%FLAVOUR%"=="" set "FLAVOUR=everyday"
if /i "%FLAVOUR%"=="everyday" (
  shift
) else if /i "%FLAVOUR%"=="guard" (
  shift
) else (
  set "FLAVOUR=everyday"
)
call "%HERE%env.bat" || exit /b 1
if "%THEFT4_DEVKITPRO%"=="" set "THEFT4_DEVKITPRO=C:/devkitPro"
set "DEVKITPRO=%THEFT4_DEVKITPRO:\=/%"
if "%THEFT4_SWITCH_JOBS%"=="" set "THEFT4_SWITCH_JOBS=2"
rem plume's Switch CMake wants a Mesa/NVK portlibs folder even without the driver linked (thirdparty/plume/CMakeLists.txt:
rem its Vulkan headers, and a libvulkan.a that must exist; PLUME_SWITCH_NVK_LINK follows REXGLUE_SWITCH_NVK, off).
if "%THEFT4_NVK_SDK%"=="" set "THEFT4_NVK_SDK=%DEVKITPRO%/portlibs/switch"
set "THEFT4_NVK_SDK=%THEFT4_NVK_SDK:\=/%"
set "ULT=%THEFT4_ULTIMATE%"
for %%F in (
  "%DEVKITPRO%\devkitA64\bin\aarch64-none-elf-g++.exe"
  "%DEVKITPRO%\libnx\switch.specs"
  "%DEVKITPRO%\libnx\lib\libdeko3d.a"
  "%DEVKITPRO%\tools\bin\elf2nro.exe"
  "%DEVKITPRO%\tools\bin\nacptool.exe"
  "%THEFT4_NVK_SDK%\include\vulkan\vulkan.h"
  "%THEFT4_NVK_SDK%\lib\libvulkan.a"
  "%ULT%\app\generated\sources.cmake"
  "%ULT%\app\out\renut_switch\deko3d_glsl.bin"
  "%ULT%\app\out\renut_switch\deko3d_glsl_switch.cpp"
  "%ULT%\app\out\renut_switch\shader_cache_switch.cpp"
  "%ULT%\app\out\renut_switch\spirv_cache.zst"
  "%ULT%\out\deko3d_tools\spirv-cross-build\spirv-cross.exe"
  "%ULT%\out\deko3d_tools\uam-build\uam.exe"
  "%ULT%\out\glslang\StandAlone\glslangValidator.exe"
) do if not exist "%%~F" (
  echo build_switch.bat: missing %%~F
  exit /b 1
)
if /i "%FLAVOUR%"=="guard" (
  set "BUILD_DIR=%ULT%\app\out\switch-guard"
  set "FLAVOUR_ARGS=-DSASRT_HOT_GUARD_FORMS=ON -DSASRT_HOT_IN_PLACE=OFF"
) else (
  set "BUILD_DIR=%ULT%\app\out\switch"
  set "FLAVOUR_ARGS=-DSASRT_HOT_GUARD_FORMS=OFF -DSASRT_HOT_IN_PLACE=ON"
)
set "COMMIT="
for /f "delims=" %%C in ('git -C "%ULT%\.." rev-parse --short^=12 HEAD') do set "COMMIT=%%C"
if "%COMMIT%"=="" (
  echo build_switch.bat: no git commit
  exit /b 1
)
git -C "%ULT%\.." diff --quiet HEAD -- ultimate tools/ultimate || set "COMMIT=%COMMIT%-dirty"
echo build_switch.bat: %FLAVOUR% build of %COMMIT% in %BUILD_DIR% (%THEFT4_SWITCH_JOBS% jobs)
cmake -S "%ULT%\app" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=%ULT%\tools\switch\cmake\switch-devkitA64.cmake" "-DDEVKITPRO=%DEVKITPRO%" ^
  "-DREXSDK_DIR=%ULT%\sdk" -DREXGLUE_SWITCH_NVK=OFF "-DREXGLUE_SWITCH_NVK_SDK=%THEFT4_NVK_SDK%" ^
  "-DPYTHON_EXECUTABLE=%THEFT4_PYTHON_FWD%" "-DPython3_EXECUTABLE=%THEFT4_PYTHON_FWD%" ^
  %FLAVOUR_ARGS% "-DTHEFT4_BUILD_COMMIT=%COMMIT%" "-DTHEFT4_BUILD_NAME=%FLAVOUR%" || exit /b 1
cmake --build "%BUILD_DIR%" -j %THEFT4_SWITCH_JOBS% %1 %2 %3 %4 %5 %6 %7 %8 %9 || exit /b 1
if not exist "%BUILD_DIR%\renut.nro" (
  echo build_switch.bat: no renut.nro in %BUILD_DIR%
  exit /b 1
)
echo build_switch.bat: built %BUILD_DIR%\renut.nro (%COMMIT%, %FLAVOUR%)
