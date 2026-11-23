@echo off
setlocal
rem Builds viewer\build\refract_viewer.exe (Visual Studio C++ tools + Android SDK CMake/Ninja).
set "SDK=%ANDROID_SDK_ROOT%"
if not defined SDK set "SDK=%ANDROID_HOME%"
if not defined SDK set "SDK=%LOCALAPPDATA%\Android\Sdk"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_vswhere
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto missing_cpp
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto missing_cpp
set "VSINSTALLDIR="
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "SDK_CMAKE=%SDK%\cmake\3.22.1\bin"
set "VS_CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VS_NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%SDK_CMAKE%\cmake.exe" set "PATH=%SDK_CMAKE%;%PATH%"
if exist "%VS_CMAKE%\cmake.exe" set "PATH=%VS_CMAKE%;%PATH%"
if exist "%SDK_CMAKE%\ninja.exe" set "PATH=%SDK_CMAKE%;%PATH%"
if exist "%VS_NINJA%\ninja.exe" set "PATH=%VS_NINJA%;%PATH%"
where cmake.exe >nul 2>nul || goto missing_cmake
where ninja.exe >nul 2>nul || goto missing_ninja
cd /d "%~dp0"
set "GENERATOR="
if exist "build\CMakeCache.txt" (
  for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_GENERATOR:INTERNAL=" build\CMakeCache.txt') do set "GENERATOR=%%B"
)
if not defined GENERATOR set "GENERATOR=Ninja"
cmake -S . -B build -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
exit /b 0

:missing_vswhere
echo Visual Studio Installer discovery tool (vswhere.exe) was not found.
exit /b 1

:missing_cpp
echo No Visual Studio installation with the x64 C++ tools was found.
exit /b 1

:missing_cmake
echo CMake was not found in the Android SDK or selected Visual Studio installation.
exit /b 1

:missing_ninja
echo Ninja was not found in the Android SDK or selected Visual Studio installation.
exit /b 1
