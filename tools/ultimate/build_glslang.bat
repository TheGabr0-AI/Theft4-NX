@echo off
rem Theft4-NX: ultimate\tools\build_glslang.bat with this project's toolchain (tools\ultimate\env.bat): builds the host
rem glslangValidator from the SDK's glslang into ultimate\out\glslang, where the app's CMake expects it
rem (BANJO_GLSLANG_VALIDATOR: the EDRAM transfer shaders of app/src/me/masseffect/shaders become SPIR-V initializers).
setlocal
call "%~dp0env.bat" || exit /b 1
cmake -S "%THEFT4_ULTIMATE%\sdk\thirdparty\glslang" -B "%THEFT4_ULTIMATE%\out\glslang" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=ON -DENABLE_HLSL=OFF -DGLSLANG_ENABLE_INSTALL=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%THEFT4_ULTIMATE%\out\glslang" --target glslangValidator -j 6 || exit /b 1
if not exist "%THEFT4_ULTIMATE%\out\glslang\StandAlone\glslangValidator.exe" exit /b 1
echo build_glslang.bat: %THEFT4_ULTIMATE%\out\glslang\StandAlone\glslangValidator.exe
