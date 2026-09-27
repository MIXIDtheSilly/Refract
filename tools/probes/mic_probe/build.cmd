@echo off
rem Builds mic_probe for the x86_64 emulator and pushes it to /data/local/tmp.
set NDK=C:\Users\mixid\Android\Sdk\ndk\27.3.13750724\toolchains\llvm\prebuilt\windows-x86_64\bin
set ADB=C:\Users\mixid\Android\Sdk\platform-tools\adb.exe
call "%NDK%\x86_64-linux-android29-clang.cmd" -O2 -o "%~dp0mic_probe" "%~dp0mic_probe.c" -laaudio -lm || exit /b 1
"%ADB%" -s emulator-5582 push "%~dp0mic_probe" /data/local/tmp/mic_probe
"%ADB%" -s emulator-5582 shell chmod 755 /data/local/tmp/mic_probe
