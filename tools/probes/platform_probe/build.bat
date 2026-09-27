@echo off
rem Builds platform_probe.exe against the official Meta Platform SDK (not included). Set META_PLATFORM_SDK,
rem or put it at external\meta-platform-sdk next to the repo folder.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined META_PLATFORM_SDK set META_PLATFORM_SDK=%~dp0..\..\..\..\external\meta-platform-sdk
set SDK=%META_PLATFORM_SDK%
cd /d "%~dp0"
cl /nologo /EHsc /O2 /MD /I "%SDK%\Include" probe.cpp "%SDK%\Windows\OVR_PlatformLoader.cpp" ^
   /Fe:platform_probe.exe /link /LIBPATH:"%SDK%\Windows" LibOVRPlatformImpl64_1.lib advapi32.lib || exit /b 1
