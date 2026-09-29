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
    [switch]$AnyCore
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
    if ($gles -notmatch 'NVIDIA' -or $gles -match 'SwiftShader|llvmpipe|softpipe|software') {
        throw "Nvidia hardware GLES required; found: $gles"
    }
    if (!$devices.Count) { throw 'Guest reports no Vulkan devices.' }
    foreach ($device in $devices) {
        $p = $device.properties
        if ($p.vendorID -ne 4318 -or $p.deviceType -eq 4 -or $p.deviceName -match 'SwiftShader|llvmpipe|software') {
            throw "Non-Nvidia/software Vulkan device: $($p.deviceName)"
        }
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
# Sets hw.gltransport in the AVD's config.ini, keeping the original once as config.ini.before-<transport>.
function Set-GlTransport {
    $avdHome = @($env:ANDROID_AVD_HOME, $(if ($env:ANDROID_USER_HOME) { Join-Path $env:ANDROID_USER_HOME 'avd' }),
                 (Join-Path $env:USERPROFILE '.android\avd')) | Where-Object { $_ -and (Test-Path "$_\$Avd.ini") } | Select-Object -First 1
    $avdPath = if ($avdHome) { (Get-Content "$avdHome\$Avd.ini" | Where-Object { $_ -match '^path=' } | Select-Object -First 1) -replace '^path=', '' }
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
function Wait-Boot($Process) {
    $deadline = (Get-Date).AddMinutes(3)
    do {
        Start-Sleep -Seconds 2
        $ErrorActionPreference = 'Continue'
        $boot = & $adb -s $serial shell getprop sys.boot_completed 2>$null
        $ErrorActionPreference = 'Stop'
        if ($boot -eq '1') { return }
        if ($Process.HasExited) { throw "Emulator exited; see $logs" }
    } while ((Get-Date) -lt $deadline)
    throw "Boot timed out; see $logs"
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
    Write-Host 'Installing the Digitalis ARM64 translator (reboots Android)'
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
        Run "$Sdk\cmdline-tools\latest\bin\sdkmanager.bat" @($image)
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
        New-Item -ItemType Directory -Force $logs | Out-Null
        Set-GlTransport
        $arguments = @('-avd', $Avd, '-port', "$Port", '-gpu', 'host', '-accel', 'on', '-no-snapshot', '-no-boot-anim', '-memory', "$MemoryMB",
                       '-writable-system')
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
            Write-Host "Guest clock is $clock (TSC failed the boot sync check); rebooting Android ($($attempt + 1)/$TscReboots)"
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
