@echo off
rem Launches Yeeps (see launch.ps1), e.g.: launch run19
set HERE=%~dp0
if "%~1"=="" (set RUN=play) else (set RUN=%~1)
shift
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%launch.ps1" -Run %RUN% %1 %2 %3 %4
