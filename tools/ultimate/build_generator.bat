@echo off
rem Theft4-NX: builds the Ultimate Base SDK's code generator (rexglue.exe) for the PC, into ultimate\sdk\out\win-amd64
rem (the path ultimate\tools\codegen.ps1 and the app's build expect). Build folder: ultimate\sdk\out\host.
rem Rule (docs/switch-port/09-rules.md section 7): no other build running. Jobs: THEFT4_BUILD_JOBS (default 6).
setlocal
call "%~dp0env.bat" || exit /b 1
if "%THEFT4_BUILD_JOBS%"=="" set "THEFT4_BUILD_JOBS=6"
set "SDK=%THEFT4_ULTIMATE%\sdk"
if not exist "%SDK%\thirdparty\CMakeLists.txt" (
  echo build_generator.bat: ultimate\sdk\thirdparty is missing: run tools\ultimate\fetch_sdk_thirdparty.ps1 first
  exit /b 1
)
if not exist "%SDK%\out\host\CMakeCache.txt" (
  cmake -S "%SDK%" -B "%SDK%\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF || exit /b 1
)
cmake --build "%SDK%\out\host" --target rexglue -j %THEFT4_BUILD_JOBS% %* || exit /b 1
if not exist "%SDK%\out\win-amd64\rexglue.exe" (
  echo build_generator.bat: built, but %SDK%\out\win-amd64\rexglue.exe is not there
  exit /b 1
)
echo build_generator.bat: %SDK%\out\win-amd64\rexglue.exe
