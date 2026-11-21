param([string]$BuildDirectory = "$PSScriptRoot\..\..\build-windows-gpu-layer")
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe' }
if (!(Test-Path $vswhere)) { throw 'Visual Studio discovery tool (vswhere.exe) was not found.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio with the x64 C++ tools was not found.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path $cmake)) { $cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source }
if (!$cmake -or !(Test-Path $cmake)) { throw 'CMake was not found in Visual Studio or on PATH.' }
& $cmake -S $PSScriptRoot -B $BuildDirectory -DREFRACT_VULKAN_HEADERS=
if ($LASTEXITCODE) { throw 'GPU layer configuration failed.' }
& $cmake --build $BuildDirectory --config Release
if ($LASTEXITCODE) { throw 'GPU layer build failed. Stop the emulator before rebuilding a loaded DLL.' }
& "$BuildDirectory\Release\refract_gpu_interop_probe.exe"
if ($LASTEXITCODE) { throw 'GPU interop probe failed.' }
