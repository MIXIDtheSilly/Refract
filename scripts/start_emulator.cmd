@echo off
rem Starts the Yeeps emulator (see start_emulator.ps1). Arguments are passed through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_emulator.ps1" %*
