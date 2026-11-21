<#
    Plays a game with Refract Native (no emulator). The launcher's counterpart of run_windows_game.ps1.
    -PcViewer: pose_input_server.py (keyboard, mouse, gamepad) + refract_viewer.exe window on this PC.
    Otherwise: the host bridge sends the game to the VR headset. Either way refract_native.exe runs the game that
    native/tools/install_apk.py prepared, and closing the viewer or bridge window ends the session.
#>
param(
    [string]$GameName,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z0-9_.]+$')][string]$Package,
    [Parameter(Mandatory)][string]$Sysroot,
    [string]$ObbDir,
    [switch]$Owned,
    [switch]$PcViewer,
    [ValidatePattern('^[1-9][0-9]{0,18}$')][string]$UserId,
    [ValidateRange(512, 4096)][int]$EyeSize = 1600,
    [ValidateRange(25, 200)][int]$RenderScale = 100,
    [switch]$SteamVrEyeSwapchains,
    [switch]$CaptureStereoFrames
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = "$root\build-native\refract_native.exe"
$sessionExe = if ($PcViewer) { "$root\viewer\build\refract_viewer.exe" } else {
    @("$root\build-windows-nvidia\host-bridge\refract-host-bridge.exe", "$root\build-windows-nvidia\host-bridge\Release\refract-host-bridge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
$appDir = Join-Path $env:LOCALAPPDATA "Refract\native\apps\$Package"
if (!(Test-Path $exe)) { throw 'Refract Native is not built. Open Settings > Setup and build it.' }
if (!(Test-Path "$root\build-native\guest\bin\refract_app")) { throw 'The ARM64 guest launcher is missing. Open Settings > Setup and build Refract Native.' }
if (!(Test-Path "$root\build-native\java\refract-android.jar")) { throw 'The Android Java shim is missing. Open Settings > Setup and build Refract Native.' }
if (!$sessionExe -or !(Test-Path $sessionExe)) { throw 'The PC viewer or host bridge is not built. Open Settings > Setup and build the Refract components.' }
if (!(Test-Path "$appDir\app.properties")) { throw 'Install the game first.' }
if (!(Test-Path "$Sysroot\system")) { throw "Android system files not found in $Sysroot. Open Settings > Setup and download them." }
# The release package carries its own Java runtime; refract_native.exe otherwise looks for one in JAVA_HOME or Program Files\Java.
if (!$env:REFRACT_JAVA_HOME -and (Test-Path "$root\jdk\bin\server\jvm.dll")) { $env:REFRACT_JAVA_HOME = "$root\jdk" }
# The runner defaults to Dynarmic; select Digitalis with REFRACT_CPU only for builds that include it.
if ([string]::IsNullOrWhiteSpace($GameName)) { $GameName = $Package }
# Games installed outside the launcher keep their OBB/asset files in Refract Native's own folder.
$defaultObb = Join-Path $env:LOCALAPPDATA "Refract\native\obb\$Package"
if (!$ObbDir -and (Test-Path $defaultObb)) { $ObbDir = $defaultObb }
# Sideloaded Android bundles may keep the game archive beside the original APK; mount it in place to avoid copying it.
if (!$ObbDir) {
    $apkEntry = Get-Content -LiteralPath "$appDir\app.properties" | Where-Object { $_ -match '^apk=' } | Select-Object -First 1
    if ($apkEntry) {
        $apkPath = $apkEntry.Substring(4)
        if (Test-Path -LiteralPath $apkPath -PathType Leaf) {
            $apkDir = [System.IO.Path]::GetDirectoryName($apkPath)
            $assetArchive = Get-ChildItem -LiteralPath $apkDir -Filter '*.hdog' -File | Select-Object -First 1
            if ($assetArchive) {
                $ObbDir = $apkDir
                Write-Host "Mounting game asset archive $($assetArchive.Name)."
            }
        }
    }
}
# Like run_windows_game.ps1: scripts\owned_games.txt lists the games the player owns.
$ownedGamesFile = Join-Path $root 'scripts\owned_games.txt'
if (!$Owned -and (Test-Path -LiteralPath $ownedGamesFile) -and
    $Package -in @(Get-Content -LiteralPath $ownedGamesFile | ForEach-Object { $_.Trim() })) { $Owned = [switch]$true }
$logs = Join-Path $root 'build-windows-game'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$title = '"' + [regex]::Replace([regex]::Replace($GameName, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'

# A leftover pose server, viewer, bridge or runner from a crashed session holds the ports the game connects to.
Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='refract_viewer.exe' OR Name='refract-host-bridge.exe' OR Name='refract_native.exe'" |
    Where-Object { $_.Name -ne 'python.exe' -or $_.CommandLine -match 'pose_input_server\.py' } |
    Where-Object { $_.CommandLine -notmatch '--probe-openxr' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$session = $null; $game = $null; $inputServer = $null
try {
    if ($PcViewer) {
        $inputServer = Start-Process python -ArgumentList "`"$root\scripts\pose_input_server.py`"", '--eye-width', $EyeSize, '--eye-height', $EyeSize, '--refresh-rate', 250 -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\input.log" -RedirectStandardError "$logs\input.err"
        $null = $inputServer.Handle
        # The game reads its eye size from the first pose connection, so the server must be listening first.
        for ($waited = 0; $waited -lt 10000 -and !$inputServer.HasExited; $waited += 250) {
            if (Get-NetTCPConnection -LocalPort 38490 -State Listen -ErrorAction SilentlyContinue) { break }
            Start-Sleep -Milliseconds 250
        }
        if ($inputServer.HasExited) { throw 'The PC input server could not start (log: build-windows-game\input.err).' }
        $shots = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'Refract'
        $session = Start-Process -FilePath $sessionExe -ArgumentList @('--title', $title, '--stats', '0', '--shots', "`"$shots`"", '--native', 'refract_native.exe') -PassThru -RedirectStandardOutput "$logs\viewer.log" -RedirectStandardError "$logs\viewer.err"
        $null = $session.Handle
        Start-Sleep -Milliseconds 800
        if ($session.HasExited) { throw 'The PC viewer could not start (log: build-windows-game\viewer.err).' }
    } else {
        $previousSteamVrEyeSwapchains = $env:REFRACT_STEAMVR_EYE_SWAPCHAINS
        $previousCapturePrefix = $env:REFRACT_CAPTURE_PREFIX
        $env:REFRACT_STEAMVR_EYE_SWAPCHAINS = if ($SteamVrEyeSwapchains) { '1' } else { '0' }
        $env:REFRACT_RENDER_SCALE = $RenderScale
        if ($CaptureStereoFrames) { $env:REFRACT_CAPTURE_PREFIX = Join-Path $logs 'stereo' }
        try { $session = Start-Process -FilePath $sessionExe -ArgumentList @('--serve-openxr', '38490', '0', $title) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\host.log" -RedirectStandardError "$logs\host.err" }
        finally {
            if ($null -eq $previousSteamVrEyeSwapchains) { Remove-Item Env:REFRACT_STEAMVR_EYE_SWAPCHAINS -ErrorAction SilentlyContinue }
            else { $env:REFRACT_STEAMVR_EYE_SWAPCHAINS = $previousSteamVrEyeSwapchains }
            if ($null -eq $previousCapturePrefix) { Remove-Item Env:REFRACT_CAPTURE_PREFIX -ErrorAction SilentlyContinue }
            else { $env:REFRACT_CAPTURE_PREFIX = $previousCapturePrefix }
            Remove-Item Env:REFRACT_RENDER_SCALE -ErrorAction SilentlyContinue
        }
        $null = $session.Handle
        # Start the game only once the bridge has an OpenXR session (it then prints the tracking origin).
        for ($waited = 0; $waited -lt 15000 -and !$session.HasExited; $waited += 250) {
            if ((Get-Content "$logs\host.err" -Raw -ErrorAction SilentlyContinue) -match 'tracking origin=') { break }
            Start-Sleep -Milliseconds 250
        }
        if ($session.HasExited) {
            $err = (Get-Content "$logs\host.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
            throw "The VR host bridge stopped: $err (log: build-windows-game\host.err)"
        }
    }

    $arguments = @('--sysroot', "`"$Sysroot`"", '--app', "`"$appDir`"")
    $arguments += '--prop', "debug.refract.platform.owned.$Package=$(if ($Owned) { 1 } else { 0 })"
    if ($UserId) { $arguments += '--prop', "debug.refract.platform.user_id=$UserId" }
    # As in run_windows_game.ps1: ASTC textures are stored as RGBA8, which overflows a 10 GB card in Batman: Arkham Shadow.
    if ($Package -eq 'com.camouflaj.manta') { $arguments += '--prop', 'debug.refract.texture_mip_limit=1' }
    # In VR the bridge sends one pose per headset frame and the game starts its frames on them, with the runtime's field of view.
    if (!$PcViewer) { $arguments += '--prop', 'debug.refract.frame_sync=1', '--prop', 'debug.refract.hfov=' }
    if ($ObbDir) { $arguments += '--mount', "`"/sdcard/Android/obb/$Package=$ObbDir`"" }
    $game = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\native.log" -RedirectStandardError "$logs\native.err"
    $null = $game.Handle
    Write-Host "$GameName | Refract Native is running$(if ($PcViewer) { ' on this PC' } else { ' in VR' }). Closing its window stops this game session."
    while (!$session.HasExited -and !$game.HasExited) { Start-Sleep -Milliseconds 500 }
    if ($game.HasExited -and !$session.HasExited -and $game.ExitCode -ne 0) {
        $err = (Get-Content "$logs\native.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
        throw "$GameName stopped (exit code $($game.ExitCode)): $err (log: build-windows-game\native.err)"
    }
} finally {
    if ($session -and !$session.HasExited) { $null = $session.CloseMainWindow(); if (!$session.WaitForExit(5000)) { $session.Kill() } }
    if ($game -and !$game.HasExited) { $game.Kill() }
    if ($inputServer -and !$inputServer.HasExited) { $inputServer.Kill() }
    Write-Host 'Refract game session stopped.'
}
