@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%~dp0..\runs\transport-test" mkdir "%~dp0..\runs\transport-test"
cl /nologo /EHsc /std:c++20 /I "%~dp0..\protocol" "%~dp0..\tests\windows_gpu_transport_smoke.cpp" "%~dp0..\protocol\image_transport.cpp" /Fe:"%~dp0..\runs\transport-test\gpu_transport_smoke.exe" /Fo:"%~dp0..\runs\transport-test\\" /link ws2_32.lib
if errorlevel 1 exit /b 1
"%~dp0..\runs\transport-test\gpu_transport_smoke.exe"
