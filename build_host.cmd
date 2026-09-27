@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Users\mixid\Android\Sdk\cmake\3.31.6\bin;%PATH%
cmake -S . -B build-windows-nvidia -G Ninja -DCMAKE_BUILD_TYPE=Release -DREFRACT_BUILD_ANDROID_RUNTIME=OFF -DREFRACT_BUILD_TESTS=OFF || exit /b 1
cmake --build build-windows-nvidia || exit /b 1
