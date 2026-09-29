param([switch]$Rebuild, [switch]$InstallDependencies)
$ErrorActionPreference = 'Stop'
$launcher = (Resolve-Path "$PSScriptRoot/../launcher").Path
if (!(Get-Command node -ErrorAction SilentlyContinue)) { throw 'Install Node.js 22 or later, then run this launcher again.' }
if ($InstallDependencies -or !(Test-Path "$launcher/node_modules/@tauri-apps/cli/package.json") -or !(Test-Path "$launcher/node_modules/react/package.json")) {
    & npm.cmd ci --prefix $launcher --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw 'Launcher dependencies could not be installed.' }
}

# The UI is compiled into the executable, so rebuild when any of its sources is newer.
# The backend (launcher/backend, launcher/core) is read from the repository at startup.
$exe = "$launcher/src-tauri/target/release/refract-launcher.exe"
$sources = @(Get-ChildItem "$launcher/src", "$launcher/src-tauri/src", "$launcher/src-tauri/capabilities", "$launcher/src-tauri/icons" -Recurse -File) +
    @(Get-Item "$launcher/index.html", "$launcher/package.json", "$launcher/vite.config.mjs", "$launcher/src-tauri/Cargo.toml", "$launcher/src-tauri/tauri.conf.json", "$launcher/src-tauri/build.rs")
$newest = ($sources | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
if ($Rebuild -or !(Test-Path $exe) -or $newest -gt (Get-Item $exe).LastWriteTime) {
    if (!(Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'Install Rust from https://rustup.rs (with the Visual Studio C++ build tools) to build the launcher.' }
    Write-Host 'Building the Refract launcher. The first build takes a few minutes.'
    Push-Location $launcher
    try {
        & npm.cmd run tauri -- build --no-bundle
        if ($LASTEXITCODE -ne 0) { throw 'The launcher could not be built. See the output above.' }
    } finally { Pop-Location }
}
Start-Process -FilePath $exe -WorkingDirectory $launcher | Out-Null
