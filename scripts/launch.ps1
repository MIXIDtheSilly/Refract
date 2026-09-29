param([Parameter(Mandatory)][string]$Run, [string]$Serial = 'emulator-5582', [switch]$InstallRuntime, [switch]$InstallPlatform, [switch]$PlatformVerbose,
      [switch]$PixelStream, [ValidateRange(10, 100)][int]$StreamScale = 100,
      [int]$EyeWidth = 1600, [int]$EyeHeight = 1600, [ValidateRange(40, 170)][int]$Fov = 109,
      [ValidateRange(40, 250)][int]$RefreshRate = 250,
      [ValidateSet('', 'two-gear', 'heavy-optimize', 'lite-translate-or-interpret', 'interpret-only')][string]$TranslatorMode = '',
      [string]$UnityArgs = '', [string]$Package = 'com.TrassGames.Yeeps',
      [switch]$RawPixels, [ValidateRange(2, 200)][int]$VideoMbps = 40,
      [string]$Activity = 'com.unity3d.player.UnityPlayerGameActivity', [switch]$UncachedBuffers)
# Relaunch Yeeps (or -Package/-Activity, e.g. com.meta.samples.NorthStar/com.meta.northstar.NorthStarActivity) under Refract: fresh logcat in runs\<Run>, controller input server, live view window.
# GPU sharing (default) needs the emulator started by start_emulator.ps1 (Refract Vulkan layer loaded);
# without the layer the runtime falls back to pixels over adb by itself.
# -PixelStream forces pixels over adb; -StreamScale shrinks that stream (percent).
# -EyeWidth/-EyeHeight set the game's render size per eye; -Fov is the horizontal
# field of view in degrees (the vertical one follows the aspect ratio).
# A real phone (-Serial not emulator-*) streams hardware-encoded H.264 (-VideoMbps) unless -RawPixels.
# -RefreshRate is the frame rate Refract paces the game to; 250 (the maximum) leaves it effectively uncapped.
# -InstallPlatform installs Refract's Meta Platform SDK stand-in (package com.oculus.horizon, built by
# Refract\platform-sdk\build_apk.ps1), which unmodified Quest APKs load instead of Horizon OS. A game is reported
# as entitled only if its package is listed in owned_games.txt; -PlatformVerbose logs every ovr_* call.
# platform_user_id.txt (optional) sets the Meta user id games see.
# The runtime Vulkan layer (tools/android_vulkan_layer.cpp, deployed by tools/android_runtime_policy.py) is enabled
# for -Package. It gives the game's CPU-written buffers cached memory instead of gfxstream's slow uncached memory
# (Batman's heavy scene 41 -> 68 fps); -UncachedBuffers turns that off.
# -TranslatorMode sets the ARM translator's (libndk_translation) berberis.mode until the next reboot; the default
# two-gear measured fastest. Translator flags are ro.berberis.flags in /system/build.prop (reboot to apply).
$ErrorActionPreference = 'Stop'
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'
$root = Split-Path $PSScriptRoot -Parent
$refract = $root
$viewer = Join-Path $root 'viewer\build\refract_viewer.exe'
$out = Join-Path $root "runs\$Run"
New-Item -ItemType Directory -Force $out | Out-Null

Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -match 'capture.py|live_view.py' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Get-Process refract_viewer -ErrorAction SilentlyContinue | Stop-Process -Force
Get-CimInstance Win32_Process -Filter "Name='adb.exe'" | Where-Object { $_.CommandLine -match 'logcat' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
# Poses + controller input come from pose_input_server.py (replaces host-bridge --serve).
Get-CimInstance Win32_Process -Filter "Name='refract-host-bridge.exe'" | Where-Object { $_.CommandLine -match '--serve 38490' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Get-CimInstance Win32_Process -Filter "Name='python.exe'" | Where-Object { $_.CommandLine -match 'pose_input_server.py' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
Start-Process python -ArgumentList "`"$PSScriptRoot\pose_input_server.py`"", '--eye-width', $EyeWidth, '--eye-height', $EyeHeight, '--refresh-rate', $RefreshRate -WindowStyle Hidden -RedirectStandardOutput "$out\input.out.txt" -RedirectStandardError "$out\input.err.txt" | Out-Null

# Stop every game we run, not only this one: a game left in the background keeps the viewer's
# shared-texture session, and the new one then gets a frame only every 2 s.
foreach ($p in @($Package, 'com.TrassGames.Yeeps', 'com.meta.samples.NorthStar', 'com.beatgames.beatsaber') | Select-Object -Unique) {
    & $adb -s $Serial shell am force-stop $p
}
if ($InstallRuntime) {
    & $adb -s $Serial install --no-incremental --force-queryable -r "$refract\build-android-runtime-windows-arm64-v8a\refract-openxr-runtime-debug.apk" | Select-Object -Last 1
}
if ($InstallPlatform) {
    & $adb -s $Serial install --no-incremental --force-queryable -r "$refract\build-platform-sdk\refract-platform-debug.apk" | Select-Object -Last 1
}
$owned = Join-Path $PSScriptRoot 'owned_games.txt'
if (Test-Path $owned) {
    foreach ($game in Get-Content $owned | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') }) {
        & $adb -s $Serial shell setprop "debug.refract.platform.owned.$game" 1
    }
}
$userIdFile = Join-Path $PSScriptRoot 'platform_user_id.txt'
if (Test-Path $userIdFile) {
    $userId = Get-Content $userIdFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') } | Select-Object -First 1
    if ($userId) { & $adb -s $Serial shell setprop debug.refract.platform.user_id $userId }
}
& $adb -s $Serial shell setprop debug.refract.platform.verbose $(if ($PlatformVerbose) { '1' } else { '0' })
& $adb -s $Serial shell setprop debug.refract.runtime_name Oculus
& $adb -s $Serial shell setprop debug.refract.composite 1  # refract_viewer decodes atlas frames; the SteamVR bridge sets 0.
& $adb -s $Serial shell setprop debug.refract.frame_sync 0  # pose_input_server's poses don't follow a display clock.
& $adb -s $Serial shell setprop debug.refract.hfov $Fov
& $adb -s $Serial shell setprop debug.refract.gpu_share $(if ($PixelStream -or -not $Serial.StartsWith('emulator-')) { '0' } else { '1' })
# Pixel stream size only (the game still renders full size). The viewer needs both eyes.
& $adb -s $Serial shell setprop debug.refract.stream_scale $StreamScale
& $adb -s $Serial shell setprop debug.refract.stream_eyes 2
if ($TranslatorMode) {
    # berberis.mode is a default_prop: only root may set it (adb root restarts adbd, so do it before the reverses).
    & $adb -s $Serial root | Out-Null
    & $adb -s $Serial wait-for-device
    & $adb -s $Serial shell setprop berberis.mode $TranslatorMode
}
$phone = -not $Serial.StartsWith('emulator-')
& $adb -s $Serial shell setprop debug.refract.video $(if ($phone -and -not $RawPixels) { 'h264' } else { "''" })
& $adb -s $Serial shell setprop debug.refract.video_mbps $VideoMbps
# A real phone talks to this PC directly over `adb reverse` like the emulator (poses + image stream),
# and must stay awake: a locked screen stops the game.
& $adb -s $Serial shell setprop debug.refract.direct_host $(if ($phone) { '1' } else { '0' })
if ($phone) {
    & $adb -s $Serial shell 'svc power stayon usb; input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard'
} else {
    # Digitalis' files live only in the /system overlay's upper layer; after a boot the first app to open
    # them gets EACCES until something else has looked them up, so read them once as shell first.
    & $adb -s $Serial shell 'cat /system/bin/arm64/app_process64 /system/bin/arm64/linker64 > /dev/null; ls /system/lib64/arm64 > /dev/null'
    # Guest kernel tuning (root; adb root restarts adbd, so before the reverses). The emulator exposes every
    # vCPU as its own package, so with TTWU_QUEUE each cross-CPU wake-up is an IPI, which is a slow
    # exit under WHPX; NO_TTWU_QUEUE cut them ~3000/s -> ~200/s and gave Batman ~+5% fps. Batman's Meta XR
    # Audio sink restarts every second and leaks ~280 16 KB blocks each time; once scudo's size class is full
    # each block is its own mapping, which hit the 65530 max_map_count after ~5 min.
    & $adb -s $Serial root | Out-Null
    & $adb -s $Serial wait-for-device
    & $adb -s $Serial shell 'echo 1048576 > /proc/sys/vm/max_map_count; mount | grep -q " /sys/kernel/debug " || mount -t debugfs debugfs /sys/kernel/debug; echo NO_TTWU_QUEUE > /sys/kernel/debug/sched/features'
    & $adb -s $Serial shell settings put global gpu_debug_app $Package
    & $adb -s $Serial shell setprop debug.refract.cached_buffer_memory $(if ($UncachedBuffers) { '0' } else { '1' })
}
& $adb -s $Serial reverse tcp:38490 tcp:38490 | Out-Null
& $adb -s $Serial reverse tcp:38491 tcp:38491 | Out-Null
& $adb -s $Serial logcat -c
Start-Process $adb -ArgumentList '-s', $Serial, 'logcat', '-v', 'threadtime' -WindowStyle Hidden -RedirectStandardOutput "$out\logcat.txt" | Out-Null
# Live window (no frames written to disk); F2 saves a screenshot into runs\screenshots.
Start-Process $viewer -ArgumentList '--shots', "`"$root\runs\screenshots`"", '--adb', "`"$adb`"", '--serial', $Serial, '--package', $Package -WorkingDirectory $PSScriptRoot -WindowStyle Hidden -RedirectStandardOutput "$out\view.out.txt" -RedirectStandardError "$out\view.err.txt" | Out-Null
Start-Sleep -Seconds 1
# System apps (safetyhub, switchaccess) sometimes crash at boot; their "Application Error" dialog keeps
# focus, and Unity then idles without ever drawing a frame. Close any such dialog first.
& $adb -s $Serial shell 'for i in 1 2 3 4 5; do dumpsys window | grep mCurrentFocus | grep -qE Application.Error\|Not.Responding || break; input keyevent KEYCODE_BACK; sleep 1; done'
# -UnityArgs is Unity's command line (e.g. '-job-worker-count 2'), passed as the 'unity' intent extra.
if ($UnityArgs) {
    & $adb -s $Serial shell "am start -n $Package/$Activity -e unity '$UnityArgs'"
} else {
    & $adb -s $Serial shell am start -n $Package/$Activity
}
