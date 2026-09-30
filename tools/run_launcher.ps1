param([switch]$Rebuild, [switch]$InstallDependencies)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$launcher = (Resolve-Path "$PSScriptRoot/../launcher").Path

function Update-Path { $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User') }
# Installs a missing tool with winget (built into Windows 11); Windows may ask for permission.
function Install-Tool([string]$Name, [string]$Id, [string[]]$Extra = @()) {
    if (!(Get-Command winget -ErrorAction SilentlyContinue)) { throw "Install $Name, then run this launcher again. (winget, which installs it automatically, is not available.)" }
    Write-Host "Installing $Name. Windows may ask for permission; this can take a few minutes."
    & winget install --exact --id $Id --silent --accept-package-agreements --accept-source-agreements --disable-interactivity @Extra
    # 0x8A15002B: already installed (an older copy that winget can't upgrade in place).
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { throw "$Name could not be installed (winget exit code $LASTEXITCODE). Install it yourself, then run this launcher again." }
    Update-Path
}

Update-Path
if (!(Get-Command node -ErrorAction SilentlyContinue)) { Install-Tool 'Node.js' 'OpenJS.NodeJS.LTS' }
if (!(Get-Command python -ErrorAction SilentlyContinue) -or ((& python --version 2>&1) -notmatch '^Python 3')) { Install-Tool 'Python' 'Python.Python.3.13' }

# A launcher left open from before keeps running its old backend; restart it when its sources changed.
$uiSources = @(Get-ChildItem "$launcher/src", "$launcher/src-tauri/src", "$launcher/src-tauri/capabilities", "$launcher/src-tauri/icons" -Recurse -File) +
    @(Get-Item "$launcher/index.html", "$launcher/package.json", "$launcher/vite.config.mjs", "$launcher/src-tauri/Cargo.toml", "$launcher/src-tauri/tauri.conf.json", "$launcher/src-tauri/build.rs")
$backendSources = @(Get-ChildItem "$launcher/backend", "$launcher/core" -Recurse -File)
$newestSource = (@($uiSources) + @($backendSources) | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
foreach ($running in @(Get-Process refract-launcher -ErrorAction SilentlyContinue)) {
    if ($Rebuild -or $running.StartTime -lt $newestSource) {
        Write-Host 'Restarting the open Refract launcher to load the updated version.'
        $null = $running.CloseMainWindow()
        if (!$running.WaitForExit(10000)) { $running.Kill() }
    }
}

if ($InstallDependencies -or !(Test-Path "$launcher/node_modules/@tauri-apps/cli/package.json") -or !(Test-Path "$launcher/node_modules/react/package.json") -or
    (Get-Item "$launcher/package-lock.json").LastWriteTime -gt (Get-Item "$launcher/node_modules" -ErrorAction SilentlyContinue).LastWriteTime) {
    Write-Host 'Installing launcher packages.'
    & npm.cmd ci --prefix $launcher --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw 'Launcher dependencies could not be installed.' }
}

# The UI is compiled into the executable, so rebuild when any of its sources is newer.
# The backend (launcher/backend, launcher/core) is read from the repository at startup.
$exe = "$launcher/src-tauri/target/release/refract-launcher.exe"
$newestUi = ($uiSources | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
if ($Rebuild -or !(Test-Path $exe) -or $newestUi -gt (Get-Item $exe).LastWriteTime) {
    if (!(Get-Command cargo -ErrorAction SilentlyContinue) -and (Test-Path "$env:USERPROFILE\.cargo\bin\cargo.exe")) { $env:Path += ";$env:USERPROFILE\.cargo\bin" }
    if (!(Get-Command cargo -ErrorAction SilentlyContinue)) { Install-Tool 'Rust' 'Rustlang.Rustup'; $env:Path += ";$env:USERPROFILE\.cargo\bin" }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere) -or !(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
        Install-Tool 'the Visual Studio C++ build tools' 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    }
    Write-Host 'Building the Refract launcher. The first build takes a few minutes.'
    Push-Location $launcher
    # Tauri prints progress on stderr; with stderr redirected, 'Stop' would turn its first line into an error.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & npm.cmd run tauri -- build --no-bundle
        if ($LASTEXITCODE -ne 0) { throw 'The launcher could not be built. See the output above.' }
    } finally { $ErrorActionPreference = $previousPreference; Pop-Location }
}
Start-Process -FilePath $exe -WorkingDirectory $launcher | Out-Null
