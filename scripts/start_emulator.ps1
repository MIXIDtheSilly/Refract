# Starts the Yeeps emulator with the Refract Vulkan layer, which shares eye textures
# with the Windows viewer (no pixel copies over adb). Then run launch.ps1.
# -Audio picks the emulator's host audio backend: the default (winaudio) plays
# silence on this PC, dsound works.
# -Cores N (2-6) starts the multi-core qemu copy made by tools\patch_emulator_cores.py;
# the stock emulator runs this PC's Android on 1 vCPU.
# -KernelArgs adds guest kernel parameters (check with adb shell cat /proc/cmdline). The default
# tsc=nowatchdog stops the guest from dropping its TSC clock for HPET mid-session (slow HPET reads
# under WHPX look like TSC drift); a boot can still land on HPET, which ensure_tsc.ps1 fixes.
# Don't use tsc=reliable: it also skips the boot-time correction of the ~1.4 s TSC offset
# between vCPUs, and the game hangs.
# idle=poll (also default) keeps idle vCPUs spinning instead of halting, so waking a game worker no longer
# needs a ~300 us IPI through qemu's userspace APIC: North Star 73-77 -> 88-89 fps, at the cost of
# 6 host threads always busy (~30% of this PC's CPU).
# The AVD's config.ini has hw.gltransport=asg (shared-memory command ring; backup config.ini.before-asg).
# The default 'pipe' made every guest GL/Vulkan flush a VM exit: Beat Saber (GLES) went 22-30 -> ~98 fps.
# The guest microphone is the Windows default recording device. Without -allow-host-audio the emulator
# opens it but feeds the guest zeros (voice chat silently sends nothing); -NoHostMic restores that.
# Check with tools\mic_probe (adb shell /data/local/tmp/mic_probe while speaking).
# -AudioLatencyMs is how much audio qemu keeps queued in DirectSound (qemu's default is 10 ms, the same as its
# mixer timer period, so any late timer tick under load plays as a crackle; AC Nexus crackled). More = later sound.
# qemu is pinned to the P-cores (i7-12700: logical processors 0-15; 16-19 are E-cores) so Windows can't move
# the vCPU threads to E-cores, e.g. while the window is minimized. -AnyCore turns that off.
param([switch]$NoGpuSharing, [switch]$Hidden, [ValidateSet('dsound', 'winaudio', 'sdl')][string]$Audio = 'dsound',
      [ValidateRange(1, 6)][int]$Cores = 1, [string]$KernelArgs = 'tsc=nowatchdog idle=poll', [switch]$NoHostMic,
      [switch]$AnyCore, [ValidateRange(10, 200)][int]$AudioLatencyMs = 50)
$emulator = 'C:\Users\mixid\Android\Sdk\emulator'
if (!$AnyCore) {
    . (Join-Path (Split-Path $PSScriptRoot -Parent) 'tools\p_core_affinity.ps1')
    # Processes started below (and the qemu that emulator.exe starts) inherit this affinity.
    [Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = [IntPtr](Get-PCoreMask)
}
$layer = Join-Path (Split-Path $PSScriptRoot -Parent) 'build-windows-gpu-layer\Release'
if (!$NoGpuSharing) {
    if (!(Test-Path "$layer\refract_gpu_layer.json")) { throw "Build the layer first: $layer" }
    # Inherited only by the emulator process started here.
    $env:VK_LAYER_PATH = $layer
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_REFRACT_gpu_share'
}
# qemu 2.12 audio options come from the environment (QEMU_<driver>_<option>); inherited by the emulator.
if ($Audio -eq 'dsound') {
    $env:QEMU_DSOUND_LATENCY_MILLIS = $AudioLatencyMs
    $env:QEMU_DSOUND_BUFSIZE_OUT = 65536  # ~340 ms of 48 kHz stereo; the default 16 KiB leaves little room above the latency.
}
$arguments = @('-avd', 'refract-google-api36', '-port', '5582', '-gpu', 'host', '-accel', 'on', '-no-snapshot',
               '-no-boot-anim', '-memory', '8192', '-writable-system', '-audio', $Audio)
if (!$NoHostMic) { $arguments += '-allow-host-audio' }
if ($KernelArgs) {
    # Must come last: everything after -qemu goes to qemu, whose -append the emulator adds to the kernel command line.
    # (An extra -smp sockets=1,cores=N here has no effect: the guest still sees one package per vCPU with its own
    # cache, so launch.ps1 sets the NO_TTWU_QUEUE scheduler feature instead.)
    $qemuArgs = @('-qemu', '-append', "`"$KernelArgs`"")
} else {
    $qemuArgs = @()
}
if ($Cores -gt 1) {
    $qemu = "$emulator\qemu\windows-x86_64\qemu-system-x86_64-multicore.exe"
    if (!(Test-Path $qemu)) { throw "Run 'python tools\patch_emulator_cores.py' first to create $qemu" }
    # What emulator.exe sets up before starting qemu itself.
    $env:PATH = "$emulator\lib64;$emulator\lib64\qt\lib;$emulator\qemu\windows-x86_64;$env:PATH"
    $env:ANDROID_EMULATOR_LAUNCHER_DIR = $emulator
    Start-Process $qemu -WindowStyle $(if ($Hidden) { 'Hidden' } else { 'Minimized' }) -ArgumentList ($arguments + @('-cores', $Cores) + $qemuArgs)
} else {
    Start-Process "$emulator\emulator.exe" -WindowStyle $(if ($Hidden) { 'Hidden' } else { 'Minimized' }) -ArgumentList ($arguments + $qemuArgs)
}
