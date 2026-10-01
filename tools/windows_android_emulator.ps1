param(
    [ValidateSet('Setup', 'Start', 'Verify', 'Install', 'Stop')][string]$Action = 'Verify',
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Avd = 'refract-google-api36',
    [ValidateRange(5554, 5682)][int]$Port = 5580,
    [ValidateSet('x86_64', 'arm64-v8a')][string]$Abi = 'x86_64',
    [ValidateRange(2048, 16384)][int]$MemoryMB = 4096,
    [ValidateSet('Default', 'Tsc', 'TscCorrected')][string]$GuestClock = 'Default',
    [string]$RuntimeApk,
    [string]$AppApk,
    [switch]$GpuSharing,
    [switch]$ShowWindow,
    # CPU tuning, the same as scripts\start_emulator.ps1 (see the notes there for the measurements).
    # -Cores > 1 needs the multi-core qemu copy from tools\patch_emulator_cores.py: on Intel + WHPX the
    # stock emulator silently runs Android on one vCPU. It is capped at half this PC's P-core threads.
    [ValidateRange(1, 6)][int]$Cores = 6,
    # tsc=nowatchdog keeps the TSC clock for the whole session; idle=poll lets idle vCPUs spin instead of
    # halting, so waking a game thread needs no ~300 us IPI exit (it keeps those host threads busy).
    [string]$KernelArgs = 'tsc=nowatchdog idle=poll',
    # hw.gltransport=asg: a shared-memory command ring instead of a VM exit on every GL/Vulkan flush.
    [ValidateSet('asg', 'pipe')][string]$GlTransport = 'asg',
    # Boots that fail the kernel's TSC sync check fall back to HPET, whose reads exit to qemu (~22% of
    # UnityMain's time); reboot up to this many times until the clocksource is the TSC.
    [ValidateRange(0, 10)][int]$TscReboots = 3,
    [switch]$AnyCore,
    # Audio as in scripts\start_emulator.ps1: the default winaudio backend plays silence on this PC, and
    # qemu's 10 ms DirectSound queue crackles under load. Without -allow-host-audio the guest mic gets zeros.
    [ValidateSet('dsound', 'winaudio', 'sdl')][string]$Audio = 'dsound',
    [ValidateRange(10, 200)][int]$AudioLatencyMs = 20,
    # DirectSound buffer size: qemu fills all of it, so once guest and host audio clocks drift apart the
    # sound runs this far behind. 64 KiB (~340 ms) made Yeeps voice chat lag badly.
    [ValidateRange(30, 340)][int]$AudioBufferMs = 40,
    [switch]$NoHostMic
)
$ErrorActionPreference = 'Stop'
if ($Port % 2) { throw 'Emulator console port must be even.' }
$adb = Join-Path $Sdk 'platform-tools\adb.exe'
$emulator = Join-Path $Sdk 'emulator\emulator.exe'
$serial = "emulator-$Port"
if (!$RuntimeApk) { $RuntimeApk = "$PSScriptRoot\..\build-android-runtime-windows-$Abi\refract-openxr-runtime-debug.apk" }
# Android 16: the Digitalis ARM64 translator in prebuilts\digitalis is built for it.
$image = 'system-images;android-36;google_apis;x86_64'
$logs = Join-Path (Split-Path $PSScriptRoot -Parent) 'build-windows-emulator'
# A progress line the launcher shows while Android starts (launcher/core/runtime.mjs reads REFRACT-STAGE lines).
function Stage([string]$Text) { Write-Host $Text; [Console]::Out.WriteLine("REFRACT-STAGE: $Text"); [Console]::Out.Flush() }
function Run([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
function Verify-Gpu {
    $gles = (& $adb -s $serial shell dumpsys SurfaceFlinger | Select-String '^GLES:') -join "`n"
    if ($LASTEXITCODE -ne 0 -or !$gles) { throw 'Cannot identify guest GLES renderer.' }
    $raw = & $adb -s $serial shell cmd gpu vkjson
    if ($LASTEXITCODE -ne 0) { throw 'Guest Vulkan query failed.' }
    $vk = ($raw -join "`n") | ConvertFrom-Json
    $devices = @($vk.devices)
    # Games need the PC's GPU; software rendering is far too slow. Refract is tuned and tested on NVIDIA.
    if ($gles -match 'SwiftShader|llvmpipe|softpipe|software') {
        throw "Android is using software graphics ($gles). Update your graphics driver and try again."
    }
    if (!$devices.Count) { throw 'Android sees no Vulkan GPU. Update your graphics driver and try again.' }
    foreach ($device in $devices) {
        $p = $device.properties
        if ($p.deviceType -eq 4 -or $p.deviceName -match 'SwiftShader|llvmpipe|software') {
            throw "Android is using a software Vulkan device ($($p.deviceName)). Update your graphics driver and try again."
        }
        if ($p.vendorID -ne 4318) { Write-Warning "Refract is tested on NVIDIA GPUs; this one is $($p.deviceName)." }
    }
    New-Item -ItemType Directory -Force $logs | Out-Null
    $gles | Set-Content "$logs\guest-gles.txt"
    $raw | Set-Content "$logs\guest-vulkan.json"
    Write-Host $gles
    $devices | ForEach-Object { Write-Host "Vulkan: $($_.properties.deviceName) (vendor $($_.properties.vendorID), type $($_.properties.deviceType))" }
}
function Verify-Abi {
    $abis = (& $adb -s $serial shell getprop ro.product.cpu.abilist) -join ''
    if ($LASTEXITCODE -ne 0 -or $Abi -notin $abis.Trim().Split(',')) {
        throw "Guest does not support requested ABI $Abi (advertised: $abis)."
    }
    if ($Abi -eq 'arm64-v8a') {
        $bridge = ((& $adb -s $serial shell getprop ro.dalvik.vm.native.bridge) -join '').Trim()
        if ($LASTEXITCODE -ne 0 -or !$bridge -or $bridge -eq '0') {
            throw 'ARM64 on this x86_64 AVD requires an enabled native bridge.'
        }
        Write-Host "ARM64 native bridge: $bridge; guest ABIs: $abis"
    }
}
# The AVD's content folder (config.ini, disk images), or $null when the AVD is not found.
function Get-AvdPath {
    $avdHome = @($env:ANDROID_AVD_HOME, $(if ($env:ANDROID_USER_HOME) { Join-Path $env:ANDROID_USER_HOME 'avd' }),
                 (Join-Path $env:USERPROFILE '.android\avd')) | Where-Object { $_ -and (Test-Path "$_\$Avd.ini") } | Select-Object -First 1
    if ($avdHome) { (Get-Content "$avdHome\$Avd.ini" | Where-Object { $_ -match '^path=' } | Select-Object -First 1) -replace '^path=', '' }
}
# Sets hw.gltransport in the AVD's config.ini, keeping the original once as config.ini.before-<transport>.
function Set-GlTransport {
    $avdPath = Get-AvdPath
    $config = if ($avdPath) { Join-Path $avdPath 'config.ini' }
    if (!$config -or !(Test-Path $config)) { Write-Warning "AVD config for $Avd not found; hw.gltransport left unchanged."; return }
    $lines = @(Get-Content $config)
    $current = ($lines | Where-Object { $_ -match '^hw\.gltransport\s*=' } | Select-Object -First 1) -replace '^hw\.gltransport\s*=\s*', ''
    if ($current -eq $GlTransport) { return }
    $backup = "$config.before-$GlTransport"
    if (!(Test-Path $backup)) { Copy-Item $config $backup }
    $lines = @($lines | Where-Object { $_ -notmatch '^hw\.gltransport\s*=' }) + "hw.gltransport=$GlTransport"
    Set-Content -Path $config -Value $lines -Encoding ascii
    Write-Host "AVD graphics transport: $(if ($current) { $current } else { 'default' }) -> $GlTransport (backup: $backup)"
}
# Why the emulator process quit, in words a player can act on (the emulator's own reason is in its log).
function Get-EmulatorFailure {
    $log = "$logs\emulator.stdout.log"
    $line = if (Test-Path $log) { Get-Content $log | Where-Object { $_ -match '^(FATAL|ERROR)\s*\|' } | Select-Object -Last 1 }
    $reason = "$line" -replace '^(FATAL|ERROR)\s*\|\s*', ''
    if ($reason -match 'multiple emulators with the same AVD') {
        return "The virtual device '$Avd' is already open in another emulator. Close that emulator and try again."
    }
    if ($reason -match 'acceleration|WHPX|hypervisor|AEHD|HAXM') {
        return "The emulator needs Windows Hypervisor Platform (Settings > Setup can turn it on): $reason"
    }
    if ($reason) { return "The emulator stopped: $($reason.Trim()) (log: $log)" }
    "The emulator stopped while starting. See $log"
}
function Wait-Boot($Process, [int]$Minutes = 3) {
    $deadline = (Get-Date).AddMinutes($Minutes)
    $offlineSince = $null
    do {
        Start-Sleep -Seconds 2
        $ErrorActionPreference = 'Continue'
        $boot = & $adb -s $serial shell getprop sys.boot_completed 2>$null
        # After a cold start adb can keep a booted emulator "offline" for minutes; reconnecting clears it.
        $offline = (& $adb devices 2>$null) -match "^$serial\s+offline"
        $ErrorActionPreference = 'Stop'
        if ($boot -eq '1') { return }
        if ($Process.HasExited) { throw (Get-EmulatorFailure) }
        if (!$offline) { $offlineSince = $null }
        elseif (!$offlineSince) { $offlineSince = Get-Date }
        elseif (((Get-Date) - $offlineSince).TotalSeconds -gt 30) { & $adb reconnect offline 2>$null | Out-Null; $offlineSince = Get-Date }
    } while ((Get-Date) -lt $deadline)
    throw "Android did not finish starting within $Minutes minutes. Close Refract and try again; if it keeps happening, restart Windows. (Logs: $logs)"
}
# The guest's clocksource name, or $null when it cannot be read (no su on this image).
function Get-Prop([string]$Name) { ((& $adb -s $serial shell getprop $Name) -join '').Trim() }
# Games need the Digitalis ARM64 translator (patched for Refract; Google's is slower and lacks the fixes).
# scripts\translator.ps1 installs it into the writable /system overlay, which only boots with -writable-system.
function Use-Digitalis($Process) {
    if ((Get-Prop ro.dalvik.vm.native.bridge) -eq 'libberberis_arm64.so') { return }
    $level = [int](Get-Prop ro.build.version.sdk)
    if ($level -lt 36) {
        throw "$Avd is Android API $level; Refract's Digitalis translator needs an Android 16 (API 36) AVD such as refract-google-api36."
    }
    Stage 'Installing the ARM translator (Android restarts)'
    & "$PSScriptRoot\..\scripts\translator.ps1" -Use digitalis -Serial $serial -Adb $adb
    Start-Sleep -Seconds 5
    Wait-Boot $Process
    if ((Get-Prop ro.dalvik.vm.native.bridge) -ne 'libberberis_arm64.so') { throw 'Digitalis did not become the native bridge; see scripts\translator.ps1.' }
}
function Get-Clocksource {
    $ErrorActionPreference = 'Continue'
    $clock = ((& $adb -s $serial shell su 0 cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>$null) -join '').Trim()
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -eq 0 -and $clock -match '^[a-z0-9_-]+$') { $clock } else { $null }
}
switch ($Action) {
    Setup {
        $sdkmanager = "$Sdk\cmdline-tools\latest\bin\sdkmanager.bat"
        if (!(Test-Path $sdkmanager)) { throw "Android SDK command-line tools are missing ($sdkmanager). Install Android Studio, or its command-line tools, first." }
        # Whoever runs Setup accepts the Android SDK licenses; sdkmanager would otherwise wait for input.
        (1..30 | ForEach-Object { 'y' }) | & $sdkmanager --licenses | Out-Null
        Run $sdkmanager @('platform-tools', 'emulator', 'build-tools;36.0.0', $image)
        $avds = & $emulator -list-avds
        if ($avds -notcontains $Avd) {
            'no' | & "$Sdk\cmdline-tools\latest\bin\avdmanager.bat" create avd --name $Avd --package $image --device pixel_2
            if ($LASTEXITCODE -ne 0) { throw 'AVD creation failed.' }
        }
        Set-GlTransport
    }
    Start {
        Run $emulator @('-accel-check')
        $devices = & $adb devices
        if ($devices -match "^$serial\s") { throw "$serial already exists; use Verify or Stop first." }
        # The emulator locks its AVD: a second copy would quit at once with a FATAL in its log.
        $other = Get-CimInstance Win32_Process -Filter "Name LIKE 'qemu-system%'" | Where-Object { $_.CommandLine -match "-avd\s+$Avd(\s|$)" } | Select-Object -First 1
        if ($other) {
            $otherPort = if ($other.CommandLine -match '-port\s+(\d+)') { $Matches[1] } else { '5554' }
            throw "The virtual device '$Avd' is already running as emulator-$otherPort. Use that emulator or close it first."
        }
        New-Item -ItemType Directory -Force $logs | Out-Null
        Set-GlTransport
        # A new AVD's first boot sets up and encrypts /data. On the multi-core qemu that stalls vCPU 0 long enough
        # for the emulator's hang detector to kill it, so the first boot runs once on the stock emulator.
        # The launcher's Setup marks the AVDs it creates (launcher/core/android_sdk.mjs).
        $avdPath = Get-AvdPath
        if ($avdPath -and (Test-Path "$avdPath\refract-first-boot-pending")) {
            Stage 'Setting up Android for the first time (a few minutes, only once)'
            $first = Start-Process $emulator -ArgumentList @('-avd', $Avd, '-port', "$Port", '-gpu', 'host', '-accel', 'on', '-no-snapshot', '-no-boot-anim',
                '-memory', "$MemoryMB", '-writable-system', '-no-window', '-crash-report-mode', 'never') `
                -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\emulator.stdout.log" -RedirectStandardError "$logs\emulator.stderr.log"
            Wait-Boot $first 10
            Remove-Item "$avdPath\refract-first-boot-pending"
            Run $adb @('-s', $serial, 'shell', 'sync')
            Run $adb @('-s', $serial, 'emu', 'kill')
            if (!$first.WaitForExit(60000)) { $first.Kill() }
            Start-Sleep -Seconds 3
        }
        $arguments = @('-avd', $Avd, '-port', "$Port", '-gpu', 'host', '-accel', 'on', '-no-snapshot', '-no-boot-anim', '-memory', "$MemoryMB",
                       '-writable-system', '-audio', $Audio,
                       # After a crash (e.g. gfxstream on North Star's exit) the next start otherwise waits
                       # on a consent dialog for the pending report, which a hidden emulator never shows.
                       '-crash-report-mode', 'never')
        if (!$NoHostMic) { $arguments += '-allow-host-audio' }
        if (!$ShowWindow) { $arguments += '-no-window' }
        $affinity = $null
        if (!$AnyCore) {
            . "$PSScriptRoot\p_core_affinity.ps1"
            $mask = Get-PCoreMask
            if ($mask) { $affinity = [IntPtr]$mask }
        }
        $pThreads = if ($affinity) { ([Convert]::ToString($affinity.ToInt64(), 2) -replace '0', '').Length } else { [Environment]::ProcessorCount }
        # With idle=poll every vCPU keeps a host thread busy; leave the rest for SteamVR and the host bridge.
        $Cores = [Math]::Max(1, [Math]::Min($Cores, [Math]::Floor($pThreads / 2)))
        $kernel = @($KernelArgs)
        if ($GuestClock -ne 'Default') {
            # Request the CPU clock, retaining Linux's stability checks.
            $arguments += '-show-kernel'
            $kernel += 'clocksource=tsc'
        }
        $kernel = ($kernel | Where-Object { $_ }) -join ' '
        $oldLayerPath = $env:VK_LAYER_PATH
        $oldLayers = $env:VK_INSTANCE_LAYERS
        $oldPath = $env:PATH
        $oldLauncherDir = $env:ANDROID_EMULATOR_LAUNCHER_DIR
        $oldAffinity = [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity
        $oldDsoundLatency = $env:QEMU_DSOUND_LATENCY_MILLIS
        $oldDsoundBuffer = $env:QEMU_DSOUND_BUFSIZE_OUT
        $launchExe = $emulator
        $multicore = Join-Path $Sdk 'emulator/qemu/windows-x86_64/qemu-system-x86_64-multicore.exe'
        if ($GuestClock -eq 'TscCorrected') {
            if ($Cores -gt 1) { Write-Warning 'Clock correction runs the stock qemu: Android gets one vCPU.' }
            $qemu = Join-Path $Sdk 'emulator/qemu/windows-x86_64/qemu-system-x86_64-headless.exe'
            $knownHash = 'DCEC1CC23AC57FF04EC748CDE7E42BFC713BF2AD532E49606A4A9332CFB94B56'
            if ((Get-FileHash -LiteralPath $qemu -Algorithm SHA256).Hash -ne $knownHash) {
                throw 'Clock correction is tested only with emulator 36.5.11 build 15261927. Use -GuestClock Default for other builds.'
            }
            $launchExe = (Resolve-Path "$PSScriptRoot/../build-whpx-clock/Release/refract_clock_launcher.exe").Path
            $clockDll = (Resolve-Path "$PSScriptRoot/../build-whpx-clock/Release/refract_whpx_clock.dll").Path
            $arguments = @(('"' + $qemu + '"'), ('"' + $clockDll + '"')) + $arguments
        } elseif ($Cores -gt 1 -and (Test-Path $multicore)) {
            $launchExe = $multicore
            $arguments += @('-cores', "$Cores")
        } elseif ($Cores -gt 1) {
            Write-Warning "No multi-core qemu ($multicore): Android may get one vCPU. Run 'python tools\patch_emulator_cores.py'."
        }
        # Must come last: everything after -qemu goes to qemu, whose -append the emulator adds to the kernel command line.
        if ($kernel) { $arguments += @('-qemu', '-append', ('"' + $kernel + '"')) }
        try {
            # qemu (and the process emulator.exe starts) inherits this affinity.
            if ($affinity) { [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = $affinity }
            if ($Audio -eq 'dsound') {
                # qemu 2.12 reads audio options from the environment (QEMU_<driver>_<option>).
                $env:QEMU_DSOUND_LATENCY_MILLIS = $AudioLatencyMs
                $env:QEMU_DSOUND_BUFSIZE_OUT = $AudioBufferMs * 192  # 48 kHz 16-bit stereo.
            }
            if ($launchExe -ne $emulator) {
                $env:ANDROID_EMULATOR_LAUNCHER_DIR = Join-Path $Sdk 'emulator'
                # What emulator.exe sets up before starting qemu itself (the windowed qemu also needs Qt).
                $env:PATH = "$Sdk\emulator;$Sdk\emulator\lib64;$Sdk\emulator\lib64\qt\lib;$Sdk\emulator\qemu\windows-x86_64;$oldPath"
            }
            if ($GpuSharing) {
                $layerPath = (Resolve-Path "$PSScriptRoot\..\build-windows-gpu-layer\Release").Path
                if (!(Test-Path "$layerPath\refract_gpu_layer.json")) { throw 'Build tools/windows_gpu_layer first.' }
                $env:VK_LAYER_PATH = $layerPath
                $env:VK_INSTANCE_LAYERS = 'VK_LAYER_REFRACT_gpu_share'
            }
            $process = Start-Process $launchExe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logs\emulator.stdout.log" -RedirectStandardError "$logs\emulator.stderr.log"
        } finally {
            [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = $oldAffinity
            $env:QEMU_DSOUND_LATENCY_MILLIS = $oldDsoundLatency
            $env:QEMU_DSOUND_BUFSIZE_OUT = $oldDsoundBuffer
            $env:VK_LAYER_PATH = $oldLayerPath
            $env:VK_INSTANCE_LAYERS = $oldLayers
            $env:PATH = $oldPath
            $env:ANDROID_EMULATOR_LAUNCHER_DIR = $oldLauncherDir
        }
        Wait-Boot $process
        try { Verify-Gpu; Verify-Abi; Use-Digitalis $process } catch {
            & $adb -s $serial emu kill | Out-Null
            throw
        }
        # Whether a boot passes the TSC sync check is luck (Linux's stability checks are never overridden).
        for ($attempt = 0; ($clock = Get-Clocksource) -and $clock -ne 'tsc' -and $attempt -lt $TscReboots; $attempt++) {
            Stage "Restarting Android for a steadier clock ($($attempt + 1)/$TscReboots)"
            Run $adb @('-s', $serial, 'reboot')
            Start-Sleep -Seconds 5
            Wait-Boot $process
        }
        if ($clock -eq 'tsc') { Write-Host 'Guest clock: TSC.' }
        elseif ($clock) { Write-Warning "Guest clock is '$clock', not the TSC; games will run slower." }
        else { Write-Warning 'Could not read the guest clocksource.' }
        Run $adb @('-s', $serial, 'reverse', 'tcp:38490', 'tcp:38490')
        Run $adb @('-s', $serial, 'reverse', 'tcp:38491', 'tcp:38491')
        Run $adb @('-s', $serial, 'shell', 'setprop', 'debug.refract.gpu_share', $(if ($GpuSharing) { '1' } else { '0' }))
        Write-Host "Ready: $serial. Images use adb reverse :38491; native pose stream uses 10.0.2.2:38490."
    }
    Verify { Verify-Gpu; Verify-Abi }
    Install {
        Verify-Gpu
        Verify-Abi
        Run $adb @('-s', $serial, 'install', '--no-incremental', '--force-queryable', '-r', $RuntimeApk)
        Run $adb @('-s', $serial, 'reverse', 'tcp:38490', 'tcp:38490')
        Run $adb @('-s', $serial, 'reverse', 'tcp:38491', 'tcp:38491')
        if ($AppApk) { Run $adb @('-s', $serial, 'install', '--no-incremental', '-r', $AppApk) }
    }
    Stop { Run $adb @('-s', $serial, 'emu', 'kill') }
}
