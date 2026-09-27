#!/bin/bash
# Copies the freshly built Digitalis translator (tools/translator/digitalis_build_host.sh) into
# prebuilts/digitalis, pushes it to the emulator and reboots Android. Run
# scripts/ensure_tsc.ps1 afterwards. The previous prebuilt is kept as <name>.before-<tag>.
# usage: digitalis_deploy.sh <tag>
set -e
tag=${1:?usage: digitalis_deploy.sh <tag>}
export MSYS_NO_PATHCONV=1
adb=/c/Users/mixid/Android/Sdk/platform-tools/adb.exe
serial=emulator-5582
bundle=$(cd "$(dirname "$0")/../../prebuilts/digitalis" && pwd -W)
lib=$bundle/system/lib64/libberberis_arm64.so

cp "$lib" "$bundle/libberberis_arm64.so.before-$tag"
wsl -d Ubuntu-24.04 -u root -- cp /root/aosp/out/target/product/emu64xa/system/lib64/libberberis_arm64.so \
    "$(wsl -d Ubuntu-24.04 wslpath -u "$lib")"
timeout 20 $adb -s $serial root >/dev/null
sleep 3
timeout 30 $adb -s $serial remount 2>&1 | tail -1
timeout 60 $adb -s $serial push "$lib" /system/lib64/libberberis_arm64.so | tail -1
timeout 20 $adb -s $serial shell 'chmod 644 /system/lib64/libberberis_arm64.so; restorecon /system/lib64/libberberis_arm64.so 2>/dev/null; sync'
timeout 20 $adb -s $serial reboot
for i in $(seq 1 12); do
    sleep 10
    if [ "$(timeout 8 $adb -s $serial shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
        echo "booted after $((i * 10)) s"
        exit 0
    fi
done
echo 'boot did not complete in 120 s' >&2
exit 1
