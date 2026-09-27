param([Parameter(Mandatory)][ValidateSet('google', 'digitalis')][string]$Use,
      [string]$Serial = 'emulator-5582', [switch]$Refresh, [switch]$NoReboot)
# Switches the emulator's ARM64 translator between Google's libndk_translation and Digitalis
# (open-source berberis, built into ..\third_party\digitalis-prebuilts). Both sets live side by side on
# /system: the active one is copied into /system/lib64/arm64, /system/bin/arm64 and
# /system/etc/ld.config.arm64.txt, and ro.dalvik.vm.native.bridge picks the host library.
# Google's original set is saved once as *.google (and pulled to scripts\stubs\google-translator).
# -Refresh re-pushes the Digitalis bundle after a rebuild. Reboots Android unless -NoReboot.
$ErrorActionPreference = 'Stop'
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'
$bundle = Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\digitalis-prebuilts\system'
$hostLib = @{ google = 'libndk_translation.so'; digitalis = 'libberberis_arm64.so' }

function Sh([string]$command) {
    $out = & $adb -s $Serial shell $command
    if ($LASTEXITCODE) { throw "adb shell failed: $command`n$out" }
    $out
}

& $adb -s $Serial root | Out-Null
& $adb -s $Serial wait-for-device
& $adb -s $Serial remount | Select-Object -Last 1

# One-time backup of Google's translator files (on the device and on this PC).
if ((Sh '[ -d /system/lib64/arm64.google ] && echo yes || echo no') -ne 'yes') {
    Sh 'cp -a /system/lib64/arm64 /system/lib64/arm64.google && cp -a /system/bin/arm64 /system/bin/arm64.google && cp -a /system/etc/ld.config.arm64.txt /system/etc/ld.config.arm64.txt.google'
    $backup = Join-Path $PSScriptRoot 'stubs\google-translator'
    New-Item -ItemType Directory -Force $backup | Out-Null
    & $adb -s $Serial pull /system/lib64/arm64.google "$backup\lib64-arm64" | Select-Object -Last 1
    & $adb -s $Serial pull /system/bin/arm64.google "$backup\bin-arm64" | Select-Object -Last 1
    & $adb -s $Serial pull /system/etc/ld.config.arm64.txt.google "$backup\ld.config.arm64.txt" | Select-Object -Last 1
    Write-Output "Saved Google's translator files to $backup"
}

# Stage the Digitalis set (host libraries go straight into /system/lib64; they do not clash).
if ($Refresh -or (Sh '[ -d /system/lib64/arm64.digitalis ] && echo yes || echo no') -ne 'yes') {
    if (-not (Test-Path "$bundle\lib64\libberberis_arm64.so")) { throw "Digitalis bundle not found at $bundle" }
    Sh 'rm -rf /system/lib64/arm64.digitalis /system/bin/arm64.digitalis'
    & $adb -s $Serial push "$bundle\lib64\arm64" /system/lib64/arm64.digitalis | Select-Object -Last 1
    & $adb -s $Serial push "$bundle\bin\arm64" /system/bin/arm64.digitalis | Select-Object -Last 1
    & $adb -s $Serial push "$bundle\etc\ld.config.arm64.txt" /system/etc/ld.config.arm64.txt.digitalis | Select-Object -Last 1
    Get-ChildItem "$bundle\lib64" -Filter 'libberberis_*.so' | ForEach-Object {
        & $adb -s $Serial push $_.FullName "/system/lib64/$($_.Name)" | Out-Null
    }
    Get-ChildItem "$bundle\bin" -Filter 'berberis_program_runner*' | ForEach-Object {
        & $adb -s $Serial push $_.FullName "/system/bin/$($_.Name)" | Out-Null
    }
    Sh 'chmod 644 /system/lib64/arm64.digitalis/* /system/lib64/libberberis_*.so /system/etc/ld.config.arm64.txt.digitalis; chmod 755 /system/bin/arm64.digitalis/* /system/bin/berberis_program_runner*'
    Write-Output 'Staged the Digitalis bundle'
}

# Activate the chosen set.
Sh "rm -rf /system/lib64/arm64 /system/bin/arm64 && cp -a /system/lib64/arm64.$Use /system/lib64/arm64 && cp -a /system/bin/arm64.$Use /system/bin/arm64 && cp /system/etc/ld.config.arm64.txt.$Use /system/etc/ld.config.arm64.txt"
Sh "sed -i 's/^ro.dalvik.vm.native.bridge=.*/ro.dalvik.vm.native.bridge=$($hostLib[$Use])/' /system/build.prop"
Sh 'restorecon -R /system/lib64/arm64 /system/bin/arm64 /system/etc/ld.config.arm64.txt /system/lib64 /system/bin 2>/dev/null; sync' | Out-Null
Write-Output "Translator set to $Use ($(Sh 'grep ^ro.dalvik.vm.native.bridge= /system/build.prop'))"
if (-not $NoReboot) {
    & $adb -s $Serial reboot
    Write-Output 'Rebooting Android; relaunch the game with scripts\launch.cmd once it is back.'
}
