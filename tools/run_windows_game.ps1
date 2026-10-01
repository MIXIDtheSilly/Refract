param(
    [string]$GameName,
    [string]$AppApk,
    [string]$RuntimeApk,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Avd = 'refract-google-api36',
    [ValidatePattern('^[a-zA-Z0-9_.]+$')][string]$Package = 'com.meta.samples.NorthStar',
    [ValidatePattern('^[a-zA-Z0-9_./]+$')][string]$Activity = 'com.meta.samples.NorthStar/com.meta.northstar.NorthStarActivity',
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [ValidateRange(5554, 5682)][int]$Port = 5580,
    [ValidateRange(2048, 16384)][int]$MemoryMB = 4096,
    [ValidateSet('Auto', 'Default', 'Tsc', 'TscCorrected')][string]$GuestClock = 'Auto',
    [ValidateSet('Auto', 'Off')][string]$UnrealMemoryPolicy = 'Auto',
    [switch]$GpuSharing,
    # The launcher passes -Owned for games Meta lists as the signed-in account's entitlements.
    [switch]$Owned,
    # -PcViewer shows the game in a window on this PC (viewer\build\refract_viewer.exe) with keyboard, mouse and
    # gamepad input (scripts\pose_input_server.py) instead of in a VR headset through the host bridge.
    [switch]$PcViewer,
    # build_host.cmd (Ninja) writes host-bridge\refract-host-bridge.exe; multi-config generators add Release\.
    [string]$HostExe
)
$ErrorActionPreference = 'Stop'
if (!$PSBoundParameters.ContainsKey('GpuSharing')) {
    $GpuSharing = Test-Path "$PSScriptRoot\..\build-windows-gpu-layer\Release\refract_gpu_layer.json"
}
if ($Activity.Split('/')[0] -ne $Package) { throw 'Activity must belong to Package.' }
if ($PcViewer) {
    $ViewerExe = "$PSScriptRoot\..\viewer\build\refract_viewer.exe"
    if (!(Test-Path $ViewerExe)) { throw 'The PC viewer is not built. Open Settings > Setup and build the Refract components.' }
    $ViewerExe = (Resolve-Path -LiteralPath $ViewerExe).Path
} else {
    if (!$HostExe) {
        $HostExe = @("$PSScriptRoot\..\build-windows-nvidia\host-bridge\refract-host-bridge.exe",
                     "$PSScriptRoot\..\build-windows-nvidia\host-bridge\Release\refract-host-bridge.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if (!$HostExe) { throw 'Build the host bridge first (build_host.cmd).' }
    }
    $HostExe = (Resolve-Path -LiteralPath $HostExe).Path
}
$adb = Join-Path $Sdk 'platform-tools\adb.exe'
$serial = "emulator-$Port"
if (!$PSBoundParameters.ContainsKey('MemoryMB') -and $Package -in 'com.zenstudios.PFXVRQuest', 'com.camouflaj.manta') { $MemoryMB = 8192 }
if ($GuestClock -eq 'Auto') {
    $GuestClock = 'Default'
    $clockTools = "$PSScriptRoot/../build-whpx-clock/Release"
    $qemu = Join-Path $Sdk 'emulator/qemu/windows-x86_64/qemu-system-x86_64-headless.exe'
    if ((Test-Path "$clockTools/refract_clock_launcher.exe") -and (Test-Path "$clockTools/refract_whpx_clock.dll") -and
        (Test-Path $qemu) -and (Get-FileHash -LiteralPath $qemu -Algorithm SHA256).Hash -eq
        'DCEC1CC23AC57FF04EC748CDE7E42BFC713BF2AD532E49606A4A9332CFB94B56') { $GuestClock = 'TscCorrected' }
}
$logs = Join-Path (Split-Path $PSScriptRoot -Parent) 'build-windows-game'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

# All ADB operations are bounded so an unresponsive guest cannot strand the window.
function Invoke-Adb([string[]]$Arguments, [int]$TimeoutMs = 10000) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $adb
    $info.Arguments = $(if ($Arguments[0] -eq 'devices') { $Arguments -join ' ' } else { (@('-s', $serial) + $Arguments) -join ' ' })
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $child = [System.Diagnostics.Process]::Start($info)
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (!$child.WaitForExit($TimeoutMs)) { $child.Kill(); throw 'ADB timed out.' }
        return @{ Code = $child.ExitCode; Text = $stdout.Result.Trim(); Error = $stderr.Result.Trim() }
    } finally { $child.Dispose() }
}

# Why the host bridge stopped, from the OpenXR error it printed, in words a player can act on.
function Get-HostFailure {
    $err = (Get-Content "$logs\host.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
    if ($err -match 'XR_ERROR_FORM_FACTOR_UNAVAILABLE') {
        return 'No VR headset is connected. Connect your headset (Meta Horizon Link or Air Link, or SteamVR) and press Play again.'
    }
    if ($err -match 'XR_ERROR_RUNTIME_UNAVAILABLE|XR_ERROR_RUNTIME_FAILURE|xrCreateInstance') {
        return 'The PC VR runtime is not running. Start Meta Horizon Link or SteamVR and press Play again.'
    }
    if ($err) { return "The VR bridge stopped: $($err.Trim()) (log: build-windows-game\host.err)" }
    'The VR bridge stopped unexpectedly. See build-windows-game\host.err.'
}

$ownsEmulator = $false
$sessionProcess = $null
$inputServer = $null
$gameStarted = $false
$closeRequest = $null
$closeReady = $null
try {
    # get-state can wait indefinitely for an absent serial on newer ADB.
    $device = Invoke-Adb @('devices')
    if ($device.Code -ne 0 -or $device.Text -notmatch ('(?m)^' + [regex]::Escape($serial) + '\s+device\s*$')) {
        & "$PSScriptRoot\windows_android_emulator.ps1" -Action Start -Sdk $Sdk -Avd $Avd -Port $Port -Abi arm64-v8a -GpuSharing:$GpuSharing -MemoryMB $MemoryMB -GuestClock $GuestClock
        $ownsEmulator = $true
    } elseif ($PSBoundParameters.ContainsKey('Avd')) {
        $runningAvd = Invoke-Adb @('emu', 'avd', 'name')
        $runningName = ($runningAvd.Text -split '\r?\n')[0].Trim()
        if ($runningAvd.Code -ne 0 -or $runningName -ne $Avd) {
            throw "$serial is running AVD '$runningName', but '$Avd' was requested. Stop that emulator first or use another port."
        }
    }
    foreach ($apk in @($RuntimeApk, $AppApk)) {
        if ([string]::IsNullOrWhiteSpace($apk)) { continue }
        $apkPath = (Resolve-Path -LiteralPath $apk).Path
        & $adb -s $serial install --no-incremental --force-queryable -r $apkPath
        if ($LASTEXITCODE -ne 0) { throw "APK install failed: $apkPath" }
    }
    $installed = Invoke-Adb @('shell', 'pm', 'path', $Package)
    if ($installed.Code -ne 0 -or $installed.Text -notmatch '^package:') { throw "$Package is not installed." }
    $ownedGamesFile = Join-Path $PSScriptRoot '..\scripts\owned_games.txt'
    $ownedGames = if (Test-Path -LiteralPath $ownedGamesFile) {
        @(Get-Content -LiteralPath $ownedGamesFile | ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith('#') })
    } else { @() }
    $ownedProp = if ($Owned -or $Package -in $ownedGames) { '1' } else { '0' }
    $ownership = Invoke-Adb @('shell', 'setprop', "debug.refract.platform.owned.$Package", $ownedProp)
    if ($ownership.Code -ne 0) { throw "Could not apply the owned-game setting for ${Package}: $($ownership.Error)" }
    # Meta's OVRPlugin rejects an otherwise functional OpenXR runtime when its
    # reported name does not identify the Oculus compatibility environment.
    $runtimeName = Invoke-Adb @('shell', 'setprop', 'debug.refract.runtime_name', 'Oculus')
    if ($runtimeName.Code -ne 0) { throw "Could not set the Quest runtime identity: $($runtimeName.Error)" }
    # Emulated ASTC textures are stored expanded to RGBA8 on the host GPU; Batman's overflowed
    # a 10 GB card. Dropping the top mip level cuts texture memory to about a quarter.
    $mipLimit = if ($Package -eq 'com.camouflaj.manta') { '1' } else { '0' }
    $mip = Invoke-Adb @('shell', 'setprop', 'debug.refract.texture_mip_limit', $mipLimit)
    if ($mip.Code -ne 0) { throw "Could not set the texture mip limit: $($mip.Error)" }
    $sessionProps = if ($PcViewer) {
        # refract_viewer decodes atlas frames (scene + panels in one image) of both eyes. pose_input_server's
        # poses don't follow a display clock. 109 degrees wide, like scripts\launch.ps1.
        [ordered]@{ 'debug.refract.composite' = '1'; 'debug.refract.frame_sync' = '0'; 'debug.refract.hfov' = '109'
                    'debug.refract.stream_eyes' = '2'; 'debug.refract.stream_scale' = '100'; 'debug.refract.direct_host' = '0' }
    } else {
        # The host bridge cannot decode refract_viewer's atlas frames; it needs panels as separate quad
        # layers, which the host OpenXR runtime composites. It sends one pose per frame of the host OpenXR
        # runtime, and the game starts its frames on them. The field of view is the runtime default ('').
        [ordered]@{ 'debug.refract.composite' = '0'; 'debug.refract.frame_sync' = '1'; 'debug.refract.hfov' = "''" }
    }
    foreach ($name in $sessionProps.Keys) {
        $set = Invoke-Adb @('shell', 'setprop', $name, $sessionProps[$name])
        if ($set.Code -ne 0) { throw "Could not set ${name}: $($set.Error)" }
    }
    $policyArgs = @("$PSScriptRoot\unreal_memory_policy.py", '--sdk', $Sdk, '--serial', $serial, '--package', $Package)
    if ($UnrealMemoryPolicy -eq 'Off') { $policyArgs += '--restore' }
    $policyJson = & python @policyArgs
    if ($LASTEXITCODE -ne 0) { throw 'Unreal memory policy failed. The game was not started.' }
    Write-Host "Unreal memory policy: $policyJson"
    # Compatibility must be supplied by Refract/Android, never by rewriting installed
    # application libraries. Keep the guest-wide mapping policy independent.
    $runtimePolicy = & python "$PSScriptRoot\android_runtime_policy.py" --sdk $Sdk --serial $serial --package $Package
    if ($LASTEXITCODE -ne 0) { throw 'Android runtime policy failed. The game was not started.' }
    Write-Host "Android runtime policy: $runtimePolicy"
    $appMetadataDir = Join-Path $logs "apps\$Package"
    $labelJson = & python "$PSScriptRoot\android_app_label.py" --sdk $Sdk --serial $serial --package $Package --icon-output "$appMetadataDir\icon.png"
    if ($LASTEXITCODE -ne 0) { throw 'Could not read the installed APK metadata.' }
    $appMetadata = $labelJson | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($GameName)) {
        $GameName = $appMetadata.label
        Write-Host "APK display name: $GameName"
    }
    $steamManifest = Join-Path $appMetadataDir 'app.vrmanifest'
    $steamIdentity = $false
    $activeRuntime = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -ErrorAction SilentlyContinue).ActiveRuntime
    if (!$PcViewer -and $activeRuntime -match 'steam') {
        $identityArgs = @("$PSScriptRoot\steamvr_app_identity.py", '--manifest', $steamManifest,
            '--package', $Package, '--name', $GameName, '--launcher', "$PSScriptRoot\run_windows_game.ps1",
            '--avd', $Avd, '--activity', $Activity)
        if ($appMetadata.icon) { $identityArgs += @('--icon', $appMetadata.icon) }
        $identityResult = & python @identityArgs
        $steamIdentity = $LASTEXITCODE -eq 0
        if ($steamIdentity) { Write-Host "SteamVR metadata: $identityResult" }
        else { Write-Warning 'SteamVR metadata registration failed; using the OpenXR application name.' }
    }
    # Quote one Windows command-line argument, including embedded quotes and
    # trailing backslashes in APK labels. Never interpret the label as code.
    $titleArgument = '"' + [regex]::Replace([regex]::Replace($GameName, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    foreach ($bridgePort in 38490,38491) {
        $reverse = Invoke-Adb @('reverse', "tcp:$bridgePort", "tcp:$bridgePort")
        if ($reverse.Code -ne 0) { throw $reverse.Error }
    }
    # A leftover pose server, viewer or bridge (a crashed session, scripts\launch.ps1) holds the pose and image
    # ports and takes the game's connection (the view then stays frozen or black).
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='refract_viewer.exe' OR Name='refract-host-bridge.exe'" |
        Where-Object { $(if ($_.Name -eq 'python.exe') { $_.CommandLine -match 'pose_input_server\.py' } else { $_.CommandLine -notmatch '--probe-openxr' }) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if ($PcViewer) {
        # Head and controller poses from the keyboard, mouse (through the viewer) and any Xbox controller.
        $inputServer = Start-Process python -ArgumentList "`"$PSScriptRoot\..\scripts\pose_input_server.py`"", '--eye-width', 1600, '--eye-height', 1600, '--refresh-rate', 250 -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\input.log" -RedirectStandardError "$logs\input.err"
        $null = $inputServer.Handle
        # The game reads its eye size from the first pose connection, so the server must be listening first.
        for ($waited = 0; $waited -lt 10000 -and !$inputServer.HasExited; $waited += 250) {
            if (Get-NetTCPConnection -LocalPort 38490 -State Listen -ErrorAction SilentlyContinue) { break }
            Start-Sleep -Milliseconds 250
        }
        if ($inputServer.HasExited) {
            $err = (Get-Content "$logs\input.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
            throw "The PC input server could not start: $err (log: build-windows-game\input.err)"
        }
        $shots = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'Refract'
        $sessionProcess = Start-Process -FilePath $ViewerExe -ArgumentList @('--title', $titleArgument, '--stats', '0', '--shots', "`"$shots`"", '--adb', "`"$adb`"", '--serial', $serial, '--package', $Package) -PassThru -RedirectStandardOutput "$logs\viewer.log" -RedirectStandardError "$logs\viewer.err"
        $null = $sessionProcess.Handle
        Start-Sleep -Milliseconds 800
        if ($sessionProcess.HasExited) {
            $err = (Get-Content "$logs\viewer.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
            throw "The PC viewer could not start: $err (log: build-windows-game\viewer.err)"
        }
    } else {
        $closeEventName = 'Local\Refract.Close.' + [guid]::NewGuid().ToString('N')
        $closeRequest = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset, $closeEventName)
        $closeReady = [System.Threading.EventWaitHandle]::new($false, [System.Threading.EventResetMode]::ManualReset, "$closeEventName.ready")
        $previousCloseEvent = $env:REFRACT_CLOSE_EVENT
        try {
            $env:REFRACT_CLOSE_EVENT = $closeEventName
            # The headset's performance panel (hold Y + B) polls Android CPU and the game's threads over adb.
            $env:REFRACT_ADB = $adb; $env:REFRACT_SERIAL = $serial; $env:REFRACT_PACKAGE = $Package
            $sessionProcess = Start-Process -FilePath $HostExe -ArgumentList @('--serve-openxr', '38490', '0', $titleArgument) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\host.log" -RedirectStandardError "$logs\host.err"
        } finally {
            $env:REFRACT_CLOSE_EVENT = $previousCloseEvent
            Remove-Item Env:REFRACT_ADB, Env:REFRACT_SERIAL, Env:REFRACT_PACKAGE -ErrorAction SilentlyContinue
        }
        $null = $sessionProcess.Handle  # Keeps ExitCode readable after the process ends.
        # Start the game only once the bridge has an OpenXR session (it then prints the tracking origin).
        for ($waited = 0; $waited -lt 15000 -and !$sessionProcess.HasExited; $waited += 250) {
            if ((Get-Content "$logs\host.err" -Raw -ErrorAction SilentlyContinue) -match 'tracking origin=') { break }
            Start-Sleep -Milliseconds 250
        }
        if ($sessionProcess.HasExited) { throw (Get-HostFailure) }
    }
    if ($steamIdentity) {
        $identityResult = & python "$PSScriptRoot\steamvr_app_identity.py" --manifest $steamManifest --package $Package --pid $sessionProcess.Id
        if ($LASTEXITCODE -eq 0) { Write-Host "SteamVR identity: $identityResult" }
        else { Write-Warning 'SteamVR process identification failed; using the OpenXR application name.' }
    }
    # A crash or "not responding" dialog shown before hide_error_dialogs was set (launcher/core/runtime.mjs) keeps
    # focus, and the game then never resumes: a black screen with no UI. Close any such dialog first.
    # (Invoke-Adb joins arguments into one command line: the script is one double-quoted argument, its pattern single-quoted for sh.)
    $null = Invoke-Adb @('shell', '"for i in 1 2 3 4 5; do dumpsys window | grep mCurrentFocus | grep -qE ''Application.Error|Not.Responding|isn.t.responding|keeps.stopping'' || break; am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS > /dev/null 2>&1; input keyevent KEYCODE_BACK; sleep 1; done"') 20000
    # Nobody can answer a permission dialog in the hidden emulator: it takes focus, the game stops responding and
    # Android closes it (Yeeps asks for the microphone). Grant the game's runtime permissions before it starts.
    $null = Invoke-Adb @('shell', 'pm', 'grant', '--all-permissions', $Package) 20000
    $launch = Invoke-Adb @('shell', 'am', 'start', '-W', '-n', $Activity) 60000
    if ($launch.Code -ne 0 -or $launch.Text -match 'Error:') { throw "Game launch failed: $($launch.Text) $($launch.Error)" }
    $gameStarted = $true
    if ($PcViewer) { Write-Host "$GameName | Playing on this PC. Closing the viewer window stops this game session." }
    else { Write-Host "$GameName | Refract is running. Closing its window stops this game session." }
    $missing = 0; $gamePid = ''
    while (!$sessionProcess.HasExited) {
        if ($closeRequest -and $closeRequest.WaitOne(0)) { break }
        $game = Invoke-Adb @('shell', 'pidof', $Package)
        if ($game.Code -eq 0 -and $game.Text) { $missing = 0; $gamePid = ($game.Text -split '\s+')[0] } else { $missing++ }
        if ($missing -ge 2) { break }
        Start-Sleep -Milliseconds 500
    }
    # The game's process went away while the bridge or viewer still ran. If it crashed, say so (and why when the
    # cause is known) instead of ending quietly and leaving the player looking at a window that never got a frame.
    if ($missing -ge 2 -and $gamePid -match '^\d+$') {
        $log = (Invoke-Adb @('logcat', '-d', "--pid=$gamePid") 20000).Text
        $crash = [regex]::Match($log, '(?m)(Fatal signal \d+.*|FATAL EXCEPTION.*|HandleFatalSignal: sig=\d+.*)$')
        if ($crash.Success -and $log -match 'VrApiLoader|VrApi Loader') {
            throw "$GameName is built on Meta's older VrApi SDK, which Refract does not support yet (only OpenXR games run)."
        }
        if ($crash.Success) { throw "$GameName crashed: $($crash.Groups[1].Value.Trim())" }
        Write-Host "$GameName closed."
    }
    # The bridge or viewer quitting by itself with an error (not the player closing it) ends the session as a failure.
    if ($sessionProcess.HasExited -and !($closeRequest -and $closeRequest.WaitOne(0)) -and $sessionProcess.ExitCode -ne 0) {
        if (!$PcViewer) { throw (Get-HostFailure) }
        $err = (Get-Content "$logs\viewer.err" -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | Select-Object -Last 1) -join ''
        throw "The PC viewer stopped (exit code $($sessionProcess.ExitCode)): $err (log: build-windows-game\viewer.err)"
    }
} finally {
    if ($gameStarted) {
        # Give the activity its normal onPause/onStop callbacks while rendering
        # and pose transport are still alive. sync alone cannot flush app memory.
        try {
            $pause = Invoke-Adb @('shell', 'am', 'start', '-W', '-a', 'android.intent.action.MAIN', '-c', 'android.intent.category.HOME') 10000
            if ($pause.Code -ne 0 -or $pause.Text -match 'Error:') { throw "Android pause failed: $($pause.Text) $($pause.Error)" }
            Start-Sleep -Seconds 2
            $flush = Invoke-Adb @('shell', 'sync')
            if ($flush.Code -ne 0) { throw "Android sync failed: $($flush.Error)" }
            Write-Host 'Android activity backgrounded and filesystem flushed before shutdown.'
        } catch { Write-Warning "Save/pause did not complete: $_" }
    }
    if ($closeReady) { $null = $closeReady.Set() }
    if ($sessionProcess -and !$sessionProcess.HasExited) {
        # WM_CLOSE lets the bridge or viewer leave its own main loop; force only after timeout.
        $null = $sessionProcess.CloseMainWindow()
        if (!$sessionProcess.WaitForExit(5000)) { $sessionProcess.Kill() }
    }
    if ($inputServer -and !$inputServer.HasExited) { $inputServer.Kill() }
    if ($ownsEmulator) {
        # North Star's force-stop can crash Gfxstream. A session-owned emulator
        # is shut down as a whole, which also terminates the Android game.
        try { $null = Invoke-Adb @('shell', 'sync'); $null = Invoke-Adb @('emu', 'kill') } catch { Write-Warning $_ }
    } elseif ($gameStarted) {
        try { $null = Invoke-Adb @('shell', 'am', 'force-stop', $Package) } catch { Write-Warning $_ }
    }
    if ($sessionProcess) { $sessionProcess.Dispose() }
    if ($inputServer) { $inputServer.Dispose() }
    if ($closeRequest) { $closeRequest.Dispose() }
    if ($closeReady) { $closeReady.Dispose() }
    Write-Host 'Refract game session stopped.'
}
