param([string]$Serial = 'emulator-5582', [int]$MaxReboots = 6)
# Waits for Android to boot and reboots it until the kernel clock is the TSC.
# At boot the kernel measures the TSC offset between vCPUs (about 1.4 s under WHPX), corrects
# it, and re-tests; if a few cycles of jitter remain it gives up on the TSC for the whole boot
# and uses HPET, whose every read exits to qemu (read_hpet was ~22% of UnityMain's time).
# Whether a boot passes is luck, so reboot until one does. Run after start_emulator.cmd.
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'

function Wait-Boot {
    & $adb -s $Serial wait-for-device
    # No double quotes: Windows PowerShell strips them from native command arguments.
    & $adb -s $Serial shell 'until getprop sys.boot_completed | grep -q 1; do sleep 1; done'
    # Reading the clocksource needs root (adb root restarts adbd).
    & $adb -s $Serial root | Out-Null
    & $adb -s $Serial wait-for-device
}

for ($attempt = 0; ; $attempt++) {
    Wait-Boot
    $clock = (& $adb -s $Serial shell cat /sys/devices/system/clocksource/clocksource0/current_clocksource).Trim()
    if ($clock -eq 'tsc') {
        Write-Output "Clocksource is tsc$(if ($attempt) { " after $attempt reboot(s)" })"
        break
    }
    if ($attempt -ge $MaxReboots) {
        Write-Warning "Clocksource is still $clock after $attempt reboots; the game will run slower"
        break
    }
    Write-Output "Clocksource is $clock (TSC failed the boot sync check); rebooting ($($attempt + 1)/$MaxReboots)"
    & $adb -s $Serial reboot
    # Give the old instance time to go away before wait-for-device.
    Start-Sleep -Seconds 5
}
