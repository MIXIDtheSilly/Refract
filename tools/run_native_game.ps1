<#
    Plays a game with Refract Native (no emulator) in the PC viewer. The launcher's counterpart of
    run_windows_game.ps1 -PcViewer: pose_input_server.py (keyboard, mouse, gamepad), refract_viewer.exe, and
    refract_native.exe running the game that native/tools/install_apk.py prepared. Closing the viewer ends the session.
#>
param(
    [string]$GameName,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_.]+$')][string]$Package,
    [Parameter(Mandatory)][string]$Sysroot,
    [string]$ObbDir,
    [switch]$Owned,
    [ValidatePattern('^[1-9][0-9]{0,18}$')][string]$UserId,
    [ValidateRange(512, 4096)][int]$EyeSize = 1600
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build-native\refract_native.exe"
$viewerExe = "$root\viewer\build\refract_viewer.exe"
$appDir = Join-Path $env:LOCALAPPDATA "Refract\native\apps\$Package"
if (!(Test-Path $exe)) { throw 'Refract Native is not built. Open Settings > Setup and build it.' }
if (!(Test-Path $viewerExe)) { throw 'The PC viewer is not built. Open Settings > Setup and build the Refract components.' }
if (!(Test-Path "$appDir\app.properties")) { throw 'Install the game first.' }
if (!(Test-Path "$Sysroot\system")) { throw "Quest firmware files not found in $Sysroot (Refract Native needs them)." }
if ([string]::IsNullOrWhiteSpace($GameName)) { $GameName = $Package }
$logs = Join-Path $root 'build-windows-game'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$title = '"' + [regex]::Replace([regex]::Replace($GameName, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'

# A leftover pose server, viewer or runner from a crashed session holds the ports the game connects to.
Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='refract_viewer.exe' OR Name='refract_native.exe'" |
    Where-Object { $_.Name -ne 'python.exe' -or $_.CommandLine -match 'pose_input_server\.py' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$viewer = $null; $game = $null
$inputServer = Start-Process python -ArgumentList "`"$root\scripts\pose_input_server.py`"", '--eye-width', $EyeSize, '--eye-height', $EyeSize, '--refresh-rate', 250 -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\input.log" -RedirectStandardError "$logs\input.err"
$null = $inputServer.Handle
try {
    # The game reads its eye size from the first pose connection, so the server must be listening first.
    for ($waited = 0; $waited -lt 10000 -and !$inputServer.HasExited; $waited += 250) {
        if (Get-NetTCPConnection -LocalPort 38490 -State Listen -ErrorAction SilentlyContinue) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($inputServer.HasExited) { throw 'The PC input server could not start (log: build-windows-game\input.err).' }
    $shots = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'Refract'
    $viewer = Start-Process -FilePath $viewerExe -ArgumentList @('--title', $title, '--stats', '0', '--shots', "`"$shots`"") -PassThru -RedirectStandardOutput "$logs\viewer.log" -RedirectStandardError "$logs\viewer.err"
    $null = $viewer.Handle
    Start-Sleep -Milliseconds 800
    if ($viewer.HasExited) { throw 'The PC viewer could not start (log: build-windows-game\viewer.err).' }

    $arguments = @('--sysroot', "`"$Sysroot`"", '--app', "`"$appDir`"")
    $arguments += '--prop', "debug.refract.platform.owned.$Package=$(if ($Owned) { 1 } else { 0 })"
    if ($UserId) { $arguments += '--prop', "debug.refract.platform.user_id=$UserId" }
    if ($ObbDir) { $arguments += '--mount', "`"/sdcard/Android/obb/$Package=$ObbDir`"" }
    $game = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\native.log" -RedirectStandardError "$logs\native.err"
    $null = $game.Handle
    Write-Host "$GameName | Playing on this PC with Refract Native. Closing the viewer window stops this game session."
    while (!$viewer.HasExited -and !$game.HasExited) { Start-Sleep -Milliseconds 500 }
    if ($game.HasExited -and !$viewer.HasExited -and $game.ExitCode -ne 0) {
        $err = (Get-Content "$logs\native.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
        throw "$GameName stopped (exit code $($game.ExitCode)): $err (log: build-windows-game\native.err)"
    }
} finally {
    if ($viewer -and !$viewer.HasExited) { $null = $viewer.CloseMainWindow(); if (!$viewer.WaitForExit(5000)) { $viewer.Kill() } }
    if ($game -and !$game.HasExited) { $game.Kill() }
    if (!$inputServer.HasExited) { $inputServer.Kill() }
    Write-Host 'Refract game session stopped.'
}
