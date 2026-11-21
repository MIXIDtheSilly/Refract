@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_vswhere

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLDIR=%%I"
if not defined VSINSTALLDIR goto missing_cpp
if not exist "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" goto missing_cpp
for /d %%I in ("%VSINSTALLDIR%\VC\Tools\MSVC\*") do set "MSVCTOOLS=%%I"
if not defined MSVCTOOLS goto missing_cpp
if not exist "%MSVCTOOLS%\bin\Hostx64\x64\cl.exe" goto missing_cpp
call "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto setup_failed

if exist "%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "PATH=%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
if exist "%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" set "PATH=%VSINSTALLDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake.exe >nul 2>nul || goto missing_cmake

set "OPENXR_SDK_DIR=%CD%\build-windows-nvidia\_deps\openxr-sdk"
set "OPENXR_HEADERS=%OPENXR_SDK_DIR%\include"
if not exist "%OPENXR_HEADERS%\openxr\openxr.h" (
    where curl.exe >nul 2>nul || goto missing_curl
    where tar.exe >nul 2>nul || goto missing_tar
    if not exist "%OPENXR_SDK_DIR%" mkdir "%OPENXR_SDK_DIR%"
    curl.exe --ssl-revoke-best-effort -fL --retry 3 "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.62.tar.gz" -o "%CD%\build-windows-nvidia\_deps\openxr-sdk-release-1.1.62.tar.gz"
    if errorlevel 1 goto openxr_download_failed
    tar.exe -xzf "%CD%\build-windows-nvidia\_deps\openxr-sdk-release-1.1.62.tar.gz" --strip-components=1 -C "%OPENXR_SDK_DIR%"
    if errorlevel 1 goto openxr_extract_failed
)

set "GENERATOR="
if exist "build-windows-nvidia\CMakeCache.txt" (
    for /f "tokens=1,* delims==" %%A in ('findstr /B /C:"CMAKE_GENERATOR:INTERNAL=" build-windows-nvidia\CMakeCache.txt') do set "GENERATOR=%%B"
)
if not defined GENERATOR where ninja.exe >nul 2>nul && set "GENERATOR=Ninja"
if not defined GENERATOR (
    for /f "tokens=1 delims=." %%V in ('"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion') do set "VSMAJOR=%%V"
    if "%VSMAJOR%"=="18" set "GENERATOR=Visual Studio 18 2026"
    if "%VSMAJOR%"=="17" set "GENERATOR=Visual Studio 17 2022"
    if "%VSMAJOR%"=="16" set "GENERATOR=Visual Studio 16 2019"
)
if not defined GENERATOR goto missing_generator

cmake.exe -S . -B build-windows-nvidia -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="%MSVCTOOLS%\bin\Hostx64\x64\cl.exe" -DREFRACT_BUILD_ANDROID_RUNTIME=OFF -DREFRACT_BUILD_TESTS=OFF -DREFRACT_OPENXR_HEADERS="%OPENXR_HEADERS%"
if errorlevel 1 exit /b 1
cmake.exe --build build-windows-nvidia --config Release
exit /b %errorlevel%

:missing_vswhere
echo Visual Studio Installer discovery tool (vswhere.exe) was not found.
echo Install Visual Studio 2019 or newer with the Desktop development with C++ workload.
exit /b 1

:missing_cpp
echo No Visual Studio installation with the x64 C++ tools was found.
echo Install Visual Studio 2019 or newer with the Desktop development with C++ workload and Windows SDK.
exit /b 1

:setup_failed
echo Failed to initialize the Visual Studio x64 build environment.
exit /b 1

:missing_cmake
echo CMake was not found on PATH or in the selected Visual Studio installation.
echo Add CMake to PATH or install the C++ CMake tools component.
exit /b 1

:missing_generator
echo Neither Ninja nor a supported Visual Studio CMake generator was found.
echo Install Ninja or use Visual Studio 2019, 2022, or 2026.
exit /b 1

:missing_curl
echo curl.exe is required to download the OpenXR headers.
exit /b 1

:missing_tar
echo tar.exe is required to extract the OpenXR headers.
exit /b 1

:openxr_download_failed
echo Failed to download the OpenXR SDK headers.
exit /b 1

:openxr_extract_failed
echo Failed to extract the OpenXR SDK headers.
exit /b 1
