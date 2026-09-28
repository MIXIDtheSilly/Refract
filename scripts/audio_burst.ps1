param([ValidateSet(4, 6, 8, 10, 22)][int]$Ms = 4, [string]$Serial = 'emulator-5582')
# Shrinks the audio buffer size apps see so Meta XR Audio (Wwise sink, used by Batman: Arkham Shadow) plays,
# while keeping the guest sound card's own buffer at the stock size. Reboots the guest; run ensure_tsc.ps1 afterwards.
#   Why: the goldfish audio HAL sizes its output as ceil(22 ms) rounded up to a multiple of 16*period_count,
#   i.e. 1088 frames at 48 kHz. AudioManager reports that as OUTPUT_FRAMES_PER_BUFFER, Oboe uses it as the
#   callback size, and EPS's standalone device (ring of 2*960 - burst frames, prefilled to 960 - burst,
#   render thread woken only when more than 960 frames are free) then never gets its render thread woken:
#   it outputs silence, the Wwise sink starves, fails after ~1 s and is re-created (leaking ~280 mallocs
#   each time). It only works for a burst of at most 480 frames (a real Quest reports 192/240).
#   -Ms 4 -> 192 frames (default, most slack for the render thread), 6 -> 320, 8 -> 384 (small gaps in Batman),
#   10 -> 512 (still too big), 22 -> stock 1088 (restore). Needs the 6-core emulator (start_emulator.cmd -Cores 6).
# The HAL opens the virtio-snd PCM with period_size = frames * period_size_multiplier / period_count, so shrinking
# the frames alone also shrinks the sound card buffer (4 ms: 2 ms periods, 8 ms total), and after a few minutes
# pcm_writei fails with EIO every ~100 ms: the host then replays stale audio (repeating, crunchy). So the script
# also sets ro.hardware.audio.tinyalsa.period_size_multiplier in /vendor/build.prop to keep the buffer near the
# stock 2176 frames (1088 * 2).
# The one patched byte is the "22" passed to util::checkAudioConfig in Device::openOutputStreamImpl of
# /vendor/lib64/hw/android.hardware.audio@7.1-impl.ranchu.so (mov esi, 22). The originals are saved next to
# them as <file>.before-burst on the first run.
# Not 'Stop': Windows PowerShell treats adb's stderr output (e.g. remount's "disabled verity") as an error.
$ErrorActionPreference = 'Continue'
$adb = 'C:\Users\mixid\Android\Sdk\platform-tools\adb.exe'
$hal = '/vendor/lib64/hw/android.hardware.audio@7.1-impl.ranchu.so'
$prop = '/vendor/build.prop'
$offset = 0x1d426          # mov esi, imm32 = BE <imm32>; file offset == vaddr in the R E segment
$frames = @{ 4 = 192; 6 = 320; 8 = 384; 10 = 512; 22 = 1088 }[$Ms]
$multiplier = [Math]::Max(2, [int][Math]::Round(2176 / $frames))
$tmp = Join-Path $env:TEMP 'hal-audio-burst.so'

& $adb -s $Serial root | Out-Null
& $adb -s $Serial wait-for-device
& $adb -s $Serial remount | Out-Null
& $adb -s $Serial shell "[ -f $hal.before-burst ] || cp -p $hal $hal.before-burst; [ -f $prop.before-burst ] || cp -p $prop $prop.before-burst"
& $adb -s $Serial pull $hal $tmp | Out-Null
$bytes = [IO.File]::ReadAllBytes($tmp)
if ($bytes[$offset] -ne 0xBE -or ($bytes[$offset + 1] -notin 4, 6, 8, 10, 22) -or $bytes[$offset + 2] -ne 0 -or $bytes[$offset + 3] -ne 0 -or $bytes[$offset + 4] -ne 0) {
    throw "Unexpected bytes at 0x$($offset.ToString('x')): $([BitConverter]::ToString($bytes[$offset..($offset + 4)])); not the expected HAL build"
}
$bytes[$offset + 1] = [byte]$Ms
[IO.File]::WriteAllBytes($tmp, $bytes)
& $adb -s $Serial push $tmp $hal | Out-Null
& $adb -s $Serial shell "chmod 644 $hal; chcon u:object_r:vendor_file:s0 $hal"
& $adb -s $Serial shell "sed -i 's/^ro.hardware.audio.tinyalsa.period_size_multiplier=.*/ro.hardware.audio.tinyalsa.period_size_multiplier=$multiplier/' $prop; grep tinyalsa $prop"
& $adb -s $Serial reboot
& $adb -s $Serial wait-for-device
# No double quotes: Windows PowerShell strips them from native command arguments.
& $adb -s $Serial shell 'until getprop sys.boot_completed | grep -q 1; do sleep 1; done; sleep 5; dumpsys media.audio_flinger | grep -m1 HAL.frame.count'
