@echo off
rem Theft4-NX: the Ultimate Base's PC toolchain (ultimate\tools\pc_env.bat) on this project's machines, without paths
rem written in the repository. Two environment variables, set once per machine (the same as tools\pc-eu-base\build.bat):
rem   THEFT4_VS_ROOT  the Visual Studio 18 folder that holds VC\Auxiliary\Build\vcvars64.bat (MSVC 14.5x, its clang 22,
rem                   CMake 4.x and Ninja 1.13 are taken from there)
rem   THEFT4_PYTHON   a Windows python.exe (3.x with the modules xxhash and Pillow), not MSYS's python3
rem Sets REX_CMAKE_COMPILERS as the base's pc_env.bat does (clang driving the MSVC ABI, lld-link) and REXGLUE_DUMP_IMAGE.
if not defined THEFT4_VS_ROOT (
  echo env.bat: set THEFT4_VS_ROOT to the Visual Studio folder that holds VC\Auxiliary\Build\vcvars64.bat
  exit /b 1
)
if not defined THEFT4_PYTHON (
  echo env.bat: set THEFT4_PYTHON to a Windows python.exe
  exit /b 1
)
rem Git stays reachable (the SDK's configure stamps versions with it).
set "THEFT4_GIT_DIR="
for /f "delims=" %%G in ('where git.exe 2^>nul') do if not defined THEFT4_GIT_DIR set "THEFT4_GIT_DIR=%%~dpG"
rem A clean PATH first: inherited from MSYS, its python3 and cmake mangle Windows paths.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%THEFT4_VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%THEFT4_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%THEFT4_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
for %%P in ("%THEFT4_PYTHON%") do set "PATH=%%~dpP;%%~dpPScripts;%PATH%"
if defined THEFT4_GIT_DIR set "PATH=%PATH%;%THEFT4_GIT_DIR%"
set "LLVM_BIN=%THEFT4_VS_ROOT%\VC\Tools\Llvm\x64\bin"
set "LLVM_BIN=%LLVM_BIN:\=/%"
set "THEFT4_PYTHON_FWD=%THEFT4_PYTHON:\=/%"
set REX_CMAKE_COMPILERS=-DCMAKE_C_COMPILER="%LLVM_BIN%/clang.exe" -DCMAKE_CXX_COMPILER="%LLVM_BIN%/clang++.exe" -DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc -DCMAKE_LINKER_TYPE=LLD -DCMAKE_RC_COMPILER="%LLVM_BIN%/llvm-rc.exe" -DCMAKE_AR="%LLVM_BIN%/llvm-ar.exe" "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DPYTHON_EXECUTABLE="%THEFT4_PYTHON_FWD%" -DPython3_EXECUTABLE="%THEFT4_PYTHON_FWD%"
rem The code generator writes the loaded game image here (post_codegen.py's ro_constants.py and flush_mode_flow.py read
rem it). Game data: never committed.
set "REXGLUE_DUMP_IMAGE=%~dp0..\..\ultimate\out\renut_image.bin"
set "THEFT4_ULTIMATE=%~dp0..\..\ultimate"
exit /b 0
