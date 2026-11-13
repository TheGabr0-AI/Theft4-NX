@echo off
rem Theft4-NX: the PC tools of the Ultimate Base's deko3d shader chain (ultimate\tools\deko3d_build_tools.sh), built with
rem this project's toolchain (tools\ultimate\env.bat: Visual Studio's clang driving the MSVC ABI, not devkitPro's MSYS2
rem clang64, which these machines do not have), into ultimate\out\deko3d_tools where the base's scripts look for them:
rem   spirv-cross-build\spirv-cross.exe   SPIR-V -> GLSL (tools\deko3d_shaders.py)
rem   uam-build\uam.exe                   GLSL -> DKSH, the base's uam (app\thirdparty\uam, SwitchWakerHD's patched
rem                                       1.1.0: the same sources the NRO compiles; tools\deko3d_variant_cache.py)
rem   uam-build\uam_dksh_test.exe         uamlib against the uam tool, DKSH byte for byte (tools\ultimate\deko3d_cache.ps1)
rem Environment, besides env.bat's THEFT4_VS_ROOT / THEFT4_PYTHON:
rem   THEFT4_SPIRV_CROSS_SRC  a SPIRV-Cross source tree at 22b22f5685d868828be01c9ac00c31902e60afd9, the commit
rem                           UnleashedRecomp-NX's thirdparty/MoltenVK/SPIRV-Cross pins (the copy the base's script
rem                           names; Apache-2.0, a PC tool only, never shipped). The tree's COMMIT file, when present,
rem                           must name that commit (git archive leaves none: write it beside CMakeLists.txt).
setlocal
call "%~dp0env.bat" || exit /b 1
set "SPVC_COMMIT=22b22f5685d868828be01c9ac00c31902e60afd9"
if not defined THEFT4_SPIRV_CROSS_SRC (
  echo build_deko3d_tools.bat: set THEFT4_SPIRV_CROSS_SRC to a SPIRV-Cross source tree at %SPVC_COMMIT%
  exit /b 1
)
if not exist "%THEFT4_SPIRV_CROSS_SRC%\CMakeLists.txt" (
  echo build_deko3d_tools.bat: no CMakeLists.txt in %THEFT4_SPIRV_CROSS_SRC%
  exit /b 1
)
if exist "%THEFT4_SPIRV_CROSS_SRC%\COMMIT" (
  findstr /x /c:"%SPVC_COMMIT%" "%THEFT4_SPIRV_CROSS_SRC%\COMMIT" >nul || (
    echo build_deko3d_tools.bat: %THEFT4_SPIRV_CROSS_SRC%\COMMIT does not name %SPVC_COMMIT%
    exit /b 1
  )
)
set "OUT=%THEFT4_ULTIMATE%\out\deko3d_tools"
set "STATIC_CRT=-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
cmake -S "%THEFT4_SPIRV_CROSS_SRC%" -B "%OUT%\spirv-cross-build" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% %STATIC_CRT% -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF || exit /b 1
cmake --build "%OUT%\spirv-cross-build" --target spirv-cross || exit /b 1
cmake -S "%THEFT4_ULTIMATE%\app\thirdparty\uam" -B "%OUT%\uam-build" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% %STATIC_CRT% -DWWHD_UAM_TEST=ON || exit /b 1
cmake --build "%OUT%\uam-build" --target uam_tool uam_dksh_test || exit /b 1
rem The MSVC-ABI uam takes the short options only (source\main.cpp): -v, not --version.
"%OUT%\uam-build\uam.exe" -v || exit /b 1
echo build_deko3d_tools.bat: built in %OUT%
