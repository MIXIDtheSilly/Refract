# Translator A/B benchmark: launches North Star and averages the game fps shown in the viewer's
# title over -Seconds, once the first scene is running. Stand still (don't touch input) while it runs.
# -SkipTsc skips ensure_tsc.ps1 (only when the guest was not rebooted since the last check).
param([int]$Seconds = 40, [int]$WarmupSeconds = 45, [string]$Run = 'ns-bench', [switch]$SkipTsc)
$ErrorActionPreference = 'Continue'
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'
if (-not $SkipTsc) { & "$PSScriptRoot\ensure_tsc.ps1" 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 1 }
& "$PSScriptRoot\launch.ps1" -Run $Run -Package com.meta.samples.NorthStar -Activity com.meta.northstar.NorthStarActivity `
    -TranslatorMode two-gear 2>&1 | Out-Null

# A system app's crash dialog sometimes takes focus after a reboot; Unity never starts behind it.
for ($t = 0; $t -lt $WarmupSeconds; $t += 5) {
    Start-Sleep 5
    $focus = & $adb -s emulator-5582 shell 'dumpsys window | grep mCurrentFocus'
    if ($focus -match 'Application Error|Application Not Responding') {
        & $adb -s emulator-5582 shell input keyevent KEYCODE_BACK
        "dismissed: $($focus.Trim())"
    }
}
$fps = for ($i = 0; $i -lt $Seconds / 2; $i++) {
    Start-Sleep 2
    $title = (Get-Process refract_viewer -ErrorAction SilentlyContinue).MainWindowTitle
    if ($title -match 'game (\d+) fps') { [int]$Matches[1] }
}
"samples: $($fps -join ' ')"
$stats = $fps | Measure-Object -Average -Minimum -Maximum
"avg {0:N1} fps (min {1}, max {2})" -f $stats.Average, $stats.Minimum, $stats.Maximum
