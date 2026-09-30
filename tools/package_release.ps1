param(
    # Where the Refract-<version>-win64 folder and zip go.
    [string]$Out = "$PSScriptRoot\..\dist",
    # Use the launcher executable already in launcher\src-tauri\target\release.
    [switch]$SkipBuild
)
# Builds a Refract package another Windows PC can run: unzip it and start Refract.exe. It carries the launcher,
# its Node.js, the built host-side programs, the Android-side APKs and the Digitalis translator. Android itself
# (emulator, system image) is downloaded on that PC by Settings > Setup, at the versions Refract is tested with.
# Everything here must already be built on this PC (build_host.cmd, viewer\build.bat, the GPU layer, the APKs).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$version = (Get-Content -Raw "$root\launcher\src-tauri\tauri.conf.json" | ConvertFrom-Json).version
$name = "Refract-$version-win64"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stage = Join-Path $Out $name
$cache = Join-Path $Out 'cache'
New-Item -ItemType Directory -Force $cache | Out-Null

function Get-Pinned([string]$Url, [string]$File, [string]$Sha256) {
    $path = Join-Path $cache $File
    if (!(Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash -ne $Sha256) {
        Invoke-WebRequest $Url -OutFile $path -UseBasicParsing
        if ((Get-FileHash $path -Algorithm SHA256).Hash -ne $Sha256) { throw "$File does not match its pinned checksum." }
    }
    $path
}

if (!$SkipBuild) {
    Write-Host 'Building the launcher'
    Push-Location "$root\launcher"
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'  # Tauri reports progress on stderr.
    try { & npm.cmd run tauri -- build --no-bundle; if ($LASTEXITCODE) { throw 'The launcher build failed.' } }
    finally { $ErrorActionPreference = $previous; Pop-Location }
}

Write-Host "Staging $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory $stage | Out-Null
function Add([string]$Source, [string]$Target = $Source) {
    $from = Join-Path $root $Source
    if (!(Test-Path $from)) { throw "Missing $Source. Build it first." }
    $to = Join-Path $stage $Target
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Copy-Item -Recurse $from $to
}

Add 'launcher\src-tauri\target\release\refract-launcher.exe' 'Refract.exe'
Get-ChildItem "$root\launcher\backend\*.mjs", "$root\launcher\core\*.mjs" | ForEach-Object { Add $_.FullName.Substring($root.Length + 1) }
Add 'launcher\inspect_apk.py'
foreach ($file in 'run_windows_game.ps1', 'windows_android_emulator.ps1', 'p_core_affinity.ps1', 'unreal_memory_policy.py',
                  'android_runtime_policy.py', 'android_app_label.py', 'steamvr_app_identity.py') { Add "tools\$file" }
foreach ($file in 'translator.ps1', 'pose_input_server.py', 'owned_games.example.txt') { Add "scripts\$file" }
Add 'build-windows-nvidia\host-bridge\refract-host-bridge.exe'
Add 'viewer\build\refract_viewer.exe'
Add 'build-windows-gpu-layer\Release\refract_gpu_layer.dll'
Add 'build-vulkan-compat\libVkLayer_REFRACT_runtime.so'
Add 'build-android-runtime-windows-arm64-v8a\refract-openxr-runtime-debug.apk'
Add 'build-android-runtime-windows-arm64-v8a\refract-systemdriver-debug.apk'
Add 'build-platform-sdk\refract-platform-debug.apk'
foreach ($item in 'system', 'NOTICE', 'README.md', 'MANIFEST.txt', 'SHA256SUMS') { Add "prebuilts\digitalis\$item" }
Add 'LICENSE'
Add 'CREDITS.md'

# The Vulkan loader resolves a relative library_path next to the manifest; the build writes an absolute one.
$manifest = Get-Content -Raw "$root\build-windows-gpu-layer\Release\refract_gpu_layer.json" | ConvertFrom-Json
$manifest.layer.library_path = '.\refract_gpu_layer.dll'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii "$stage\build-windows-gpu-layer\Release\refract_gpu_layer.json"

# Node.js runs the launcher backend (Refract.exe prefers node\node.exe beside it).
$node = (Get-Command node -ErrorAction SilentlyContinue).Source
if (!$node) { $node = "$env:ProgramFiles\nodejs\node.exe" }
$nodeVersion = (& $node --version).Trim()
New-Item -ItemType Directory "$stage\node" | Out-Null
Copy-Item $node "$stage\node\node.exe"
Invoke-WebRequest "https://raw.githubusercontent.com/nodejs/node/$nodeVersion/LICENSE" -OutFile "$stage\node\LICENSE" -UseBasicParsing

# The host bridge needs the Visual C++ runtime (redistributable app-locally) and an OpenXR loader: SteamVR has
# one, Meta Horizon Link does not.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { Copy-Item (Join-Path $crt.FullName $dll) "$stage\build-windows-nvidia\host-bridge\" }
$loaderZip = Get-Pinned 'https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.63/openxr_loader_windows-1.1.63.zip' `
    'openxr_loader_windows-1.1.63.zip' '01C631AEABBFE0879540F77EF833416C532A20746285B494630160C23588B771'
$loader = Join-Path $cache 'openxr_loader'
if (Test-Path $loader) { Remove-Item -Recurse -Force $loader }
Expand-Archive $loaderZip $loader
Copy-Item "$loader\x64\bin\openxr_loader.dll" "$stage\build-windows-nvidia\host-bridge\"
Copy-Item "$loader\share\doc\openxr\LICENSE" "$stage\build-windows-nvidia\host-bridge\openxr_loader.LICENSE.txt"

Copy-Item "$PSScriptRoot\package_readme.txt" "$stage\README.txt"

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Write-Host "Zipping $zip"
# .NET's ZipFile writes standard '/' entry names; Windows PowerShell's Compress-Archive writes '\', which some unzip tools mangle.
[AppContext]::SetSwitch('Switch.System.IO.Compression.ZipFile.UseBackslash', $false)  # .NET Framework's legacy default.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host ("Done: {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
