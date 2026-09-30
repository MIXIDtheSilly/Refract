"""Apply emulator configuration without opening or rewriting application files."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', required=True)
    args = parser.parse_args()
    import re
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+', args.package):
        parser.error('Invalid Android package')
    adb = [str(args.sdk / 'platform-tools/adb.exe'), '-s', args.serial]
    def run(*parts):
        return subprocess.run([*adb, *parts], check=True, capture_output=True,
                              text=True, timeout=120).stdout.strip()
    if run('shell', 'getprop ro.hardware') != 'ranchu':
        print(json.dumps({'status': 'skipped', 'reason': 'Not the Refract emulator'})); return
    # Games run on the Digitalis ARM64 translator (Refract's patched build in prebuilts/digitalis).
    if run('shell', 'getprop ro.dalvik.vm.native.bridge') != 'libberberis_arm64.so':
        raise RuntimeError('The emulator is not using the Digitalis translator. Boot it with -writable-system '
                           'and run scripts/translator.ps1 -Use digitalis (Android 16 / API 36 AVDs only).')
    # Digitalis' files live only in the /system overlay's upper layer; after a boot the first app to
    # open them gets EACCES until something else has looked them up, so read them once as shell first.
    run('shell', 'cat /system/bin/arm64/app_process64 /system/bin/arm64/linker64 > /dev/null; ls /system/lib64/arm64 > /dev/null')
    run('root'); run('wait-for-device')
    limit = int(run('shell', 'cat /proc/sys/vm/max_map_count'))
    if limit < 1048576:
        run('shell', 'echo 1048576 > /proc/sys/vm/max_map_count')
        limit = int(run('shell', 'cat /proc/sys/vm/max_map_count'))
        if limit < 1048576: raise RuntimeError('Memory mapping policy did not apply')
    # The emulator exposes every vCPU as its own package, so with TTWU_QUEUE each cross-CPU wake-up
    # is an IPI, a slow exit under WHPX (~3000/s -> ~200/s, Batman ~+5% fps). Resets at every boot.
    # A performance setting only: a kernel without it must not stop the game.
    features = run('shell', 'mount | grep -q " /sys/kernel/debug " || mount -t debugfs debugfs /sys/kernel/debug; '
                   'echo NO_TTWU_QUEUE > /sys/kernel/debug/sched/features; cat /sys/kernel/debug/sched/features; true')
    scheduler = 'NO_TTWU_QUEUE' if 'NO_TTWU_QUEUE' in features.split() else 'unchanged'
    root = Path(__file__).resolve().parents[1]
    source = root / 'tools/android_vulkan_layer.cpp'
    header = root / 'tools/vulkan_descriptor_template.h'
    output = root / 'build-vulkan-compat/libVkLayer_REFRACT_runtime.so'
    output.parent.mkdir(exist_ok=True)
    # A packaged Refract ships only the built layer; a source checkout rebuilds it when the source changes.
    if not output.exists() or (source.exists() and output.stat().st_mtime < max(source.stat().st_mtime, header.stat().st_mtime)):
        compilers = sorted((args.sdk / 'ndk').glob('*/toolchains/llvm/prebuilt/windows-x86_64/bin/x86_64-linux-android29-clang++.cmd'),
                           key=lambda p: tuple(int(n) for n in p.parents[5].name.split('.')))
        if not compilers: raise RuntimeError('Android NDK required for the runtime Vulkan layer')
        subprocess.run([str(compilers[-1]), '-std=c++17', '-shared', '-fPIC', '-O2', '-static-libstdc++',
                        '-Wl,-Bsymbolic', str(source), '-llog', '-o', str(output)], check=True, timeout=120)
    directory = '/data/local/debug/vulkan'
    remote = directory + '/libVkLayer_REFRACT_runtime.so'
    expected = hashlib.sha256(output.read_bytes()).hexdigest()
    run('shell', 'mkdir -p ' + directory)
    current = run('shell', 'if [ -f ' + remote + ' ]; then sha256sum ' + remote + '; fi').split()
    if not current or current[0] != expected:
        run('push', str(output), remote + '.new')
        if run('shell', 'sha256sum ' + remote + '.new').split()[0] != expected:
            raise RuntimeError('Runtime Vulkan layer transfer verification failed')
        run('shell', 'mv -f ' + remote + '.new ' + remote)
    # System debug layers need a readable/executable native-library label on
    # this userdebug image. Only our directory and library are relabeled.
    paths = '/data/local/debug ' + directory + ' ' + remote
    run('shell', 'chmod 755 ' + paths + ' && chcon u:object_r:apk_data_file:s0 ' + paths)
    run('shell', 'settings put global enable_gpu_debug_layers 1')
    run('shell', 'settings put global gpu_debug_app ' + args.package)
    run('shell', 'settings put global gpu_debug_layers VK_LAYER_REFRACT_runtime')
    run('shell', 'settings delete global gpu_debug_layer_app')
    # CPU-written buffers get cached memory instead of gfxstream's slow uncached memory
    # (Batman's heavy scene 41 -> 68 fps). Read by the layer when the game creates its Vulkan device.
    run('shell', 'setprop debug.refract.cached_buffer_memory 1')
    # ETC2/EAC/ASTC textures become BC, which the host GPU samples natively (no decompressed copy in VRAM).
    run('shell', 'setprop debug.refract.transcode_textures 1')
    run('shell', 'sync')
    print(json.dumps({'status': 'ready', 'max_map_count': limit, 'scheduler': scheduler, 'translator': 'digitalis',
                      'vulkan_layer': remote}))


if __name__ == '__main__': main()
