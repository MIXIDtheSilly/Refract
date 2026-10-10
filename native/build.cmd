@echo off
rem Builds refract_native.exe into Refract\build-native.
setlocal
set VSCMD_SKIP_SENDTELEMETRY=1
if not defined VSINSTALL set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined ANDROID_SDK set "ANDROID_SDK=%USERPROFILE%\Android\Sdk"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ANDROID_SDK%\cmake\3.31.6\bin;%PATH%"
set "SRC=%~dp0."
set "OUT=%~dp0..\build-native"
python "%SRC%\tools\gen_thunks.py" || exit /b 1
if not exist "%OUT%\build.ninja" (
  cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit /b 1
)
cmake --build "%OUT%" --target refract_native %*
