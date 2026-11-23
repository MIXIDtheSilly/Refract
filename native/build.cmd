@echo off
setlocal
set "SRC=%~dp0."
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\build-native"
echo Refract source: "%ROOT%"
if not defined REFRACT_EXTERNAL_SRC set "REFRACT_EXTERNAL_SRC=%ROOT%\..\external\src"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_vswhere
echo Finding Visual Studio with vswhere...

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto missing_cpp
echo Visual Studio: "%VSROOT%"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto missing_cpp
set "VSINSTALLDIR="
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto setup_failed
echo Checking Visual Studio compiler...
where cl.exe
if errorlevel 1 goto missing_cpp

set "CMAKE_BIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA_BIN=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%CMAKE_BIN%\cmake.exe" set "PATH=%CMAKE_BIN%;%PATH%"
if exist "%NINJA_BIN%\ninja.exe" set "PATH=%NINJA_BIN%;%PATH%"
where cmake.exe >nul 2>nul || goto missing_cmake
where ninja.exe >nul 2>nul || goto missing_ninja

set "REFRACT_JDK=%ROOT%\jdk"
echo Checking JDK headers in "%REFRACT_JDK%"
if not exist "%REFRACT_JDK%\include\jni.h" if defined JAVA_HOME set "REFRACT_JDK=%JAVA_HOME%"
if not exist "%REFRACT_JDK%\include\jni.h" set "REFRACT_JDK=C:\Program Files\Java\jdk-27"
if not exist "%REFRACT_JDK%\include\jni.h" goto missing_jdk
set "JAVA_HOME=%REFRACT_JDK%"
set "PATH=%REFRACT_JDK%\bin;%PATH%"

if not exist "%REFRACT_EXTERNAL_SRC%\dynarmic\CMakeLists.txt" goto missing_dynarmic
if not exist "%REFRACT_EXTERNAL_SRC%\boost\boost\version.hpp" goto missing_boost
if not exist "%REFRACT_EXTERNAL_SRC%\astc-encoder\Source\astcenc_entry.cpp" goto missing_astc
if not exist "%REFRACT_EXTERNAL_SRC%\Vulkan-Headers\include\vulkan\vulkan.h" goto missing_vulkan

if not defined ANDROID_SDK_ROOT if defined ANDROID_HOME set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if not defined ANDROID_SDK_ROOT set "ANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk"
set "REFRACT_NDK=%ANDROID_SDK_ROOT%\ndk\27.3.13750724"
if not exist "%REFRACT_NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe" goto missing_ndk
set "GIT_BASH=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%GIT_BASH%" set "GIT_BASH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not exist "%GIT_BASH%" goto missing_git_bash

python "%SRC%\tools\gen_thunks.py"
if errorlevel 1 exit /b 1

echo Building ARM64 guest launcher and shim libraries...
set "NDK=%REFRACT_NDK%"
"%GIT_BASH%" --noprofile --norc "%SRC%\guest\build.sh"
if errorlevel 1 exit /b 1

echo Building Android framework shim...
set "ANDROID_JAR=%ANDROID_SDK_ROOT%\platforms\android-34\android.jar"
if not exist "%ANDROID_JAR%" goto missing_android_platform
if not defined REFRACT_SYSROOT set "REFRACT_SYSROOT=%OUT%\sysroot"
if not exist "%REFRACT_SYSROOT%\system\framework\framework.jar" goto missing_sysroot
if not exist "%REFRACT_SYSROOT%\apex\com.android.art\javalib\core-libart.jar" goto missing_sysroot
set "DEX2JAR=%ROOT%\tools\dex2jar\d2j-dex2jar.bat"
if not exist "%DEX2JAR%" set "DEX2JAR=%ROOT%\..\external\dex2jar\d2j-dex2jar.bat"
if not exist "%DEX2JAR%" goto missing_dex2jar
"%GIT_BASH%" --noprofile --norc "%SRC%\java\build.sh"
if errorlevel 1 exit /b 1

rem The Digitalis CPU backend needs clang-cl; without it only Dynarmic is built.
if not defined REFRACT_LLVM set "REFRACT_LLVM=%ROOT%\..\external\llvm"
if exist "%REFRACT_LLVM%\bin\clang-cl.exe" (
  call "%SRC%\digitalis\build.cmd"
  if errorlevel 1 exit /b 1
)

if not exist "%OUT%\CMakeCache.txt" (
  cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DREFRACT_JDK=%REFRACT_JDK%" "-DREFRACT_EXTERNAL_SRC=%REFRACT_EXTERNAL_SRC%"
) else (
  cmake -S "%SRC%" -B "%OUT%" "-DREFRACT_JDK=%REFRACT_JDK%" "-DREFRACT_EXTERNAL_SRC=%REFRACT_EXTERNAL_SRC%"
)
if errorlevel 1 exit /b 1
cmake --build "%OUT%" --target refract_native %*
exit /b %errorlevel%

:missing_vswhere
echo Visual Studio Installer discovery tool (vswhere.exe) was not found.
echo Install Visual Studio 2022 or newer with the Desktop development with C++ workload.
exit /b 1

:missing_cpp
echo No Visual Studio installation with the x64 C++ tools was found.
echo Install Visual Studio 2022 or newer with the Desktop development with C++ workload and Windows SDK.
exit /b 1

:setup_failed
echo Failed to initialize the Visual Studio x64 build environment.
exit /b 1

:missing_cmake
echo CMake was not found in the selected Visual Studio installation.
echo Install the C++ CMake tools component.
exit /b 1

:missing_ninja
echo Ninja was not found in the selected Visual Studio installation.
echo Install the C++ CMake tools component.
exit /b 1

:missing_jdk
echo JDK 27 headers were not found.
echo Install Temurin 27, or put its extracted JDK in the repository's jdk folder.
exit /b 1

:missing_dynarmic
echo Dynarmic source was not found in "%REFRACT_EXTERNAL_SRC%".
echo Install the native source dependencies under external\src.
exit /b 1

:missing_boost
echo Boost headers were not found in "%REFRACT_EXTERNAL_SRC%\boost".
echo Install the native source dependencies under external\src.
exit /b 1

:missing_astc
echo astc-encoder source was not found in "%REFRACT_EXTERNAL_SRC%\astc-encoder".
echo Install the native source dependencies under external\src.
exit /b 1

:missing_vulkan
echo Vulkan-Headers source was not found in "%REFRACT_EXTERNAL_SRC%\Vulkan-Headers".
echo Install the native source dependencies under external\src.
exit /b 1

:missing_ndk
echo Android NDK 27.3.13750724 was not found in "%ANDROID_SDK_ROOT%".
echo Install it from Settings ^> Setup before building Refract Native.
exit /b 1

:missing_git_bash
echo Git Bash was not found.
echo Install Git for Windows to build the ARM64 guest launcher.
exit /b 1

:missing_android_platform
echo Android platform 34 was not found at "%ANDROID_JAR%".
echo Install it from Settings ^> Setup before building Refract Native.
exit /b 1

:missing_sysroot
echo Android framework JARs were not found in "%REFRACT_SYSROOT%".
echo Download Android system files from Settings ^> Setup before building Refract Native.
exit /b 1

:missing_dex2jar
echo dex2jar was not found.
echo Install the game conversion tools from Settings ^> Setup before building Refract Native.
exit /b 1
