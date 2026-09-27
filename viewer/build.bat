@echo off
rem Builds viewer\build\refract_viewer.exe (Visual Studio C++ tools + Android SDK cmake/ninja).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Users\mixid\Android\Sdk\cmake\3.22.1\bin;%PATH%
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
