#!/usr/bin/env bash
# Linux counterpart of tools/windows_android_emulator.ps1: the Android Emulator on KVM, drawing with this PC's own
# GPU driver (Nvidia's included) through Gfxstream. Waydroid (scripts/waydroid.sh) is lighter but can only use
# Mesa GPUs; the emulator forwards Android's GLES/Vulkan calls to the host driver, so it works on any GPU.
#
#   tools/linux_android_emulator.sh setup    Downloads the pinned Android pieces and a JDK, creates the AVD.
#   tools/linux_android_emulator.sh start    Starts Android hidden, checks its GPU, installs the Digitalis ARM64
#                                            translator and the Quest identity when missing, then Refract's APKs.
#   tools/linux_android_emulator.sh verify   Checks the running Android's GPU and ABIs.
#   tools/linux_android_emulator.sh stop
#
# Environment: ANDROID_HOME (default ~/Android/Sdk), REFRACT_AVD (refract-google-api36), REFRACT_PORT (5580),
# REFRACT_MEMORY_MB (8192), REFRACT_CORES (6), REFRACT_SHOW_WINDOW=1 for an Android window, REFRACT_GPU_SHARING=0
# to stream read-back pixels instead of sharing GPU textures, REFRACT_ANY_CORE=1 to let Android use E-cores,
# REFRACT_KERNEL_ARGS for extra Android kernel arguments.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
JDK="${REFRACT_JDK:-$(dirname "$SDK")/jdk-21}"
AVD="${REFRACT_AVD:-refract-google-api36}"
PORT="${REFRACT_PORT:-5580}"
MEMORY_MB="${REFRACT_MEMORY_MB:-8192}"
CORES="${REFRACT_CORES:-6}"
GPU_SHARING="${REFRACT_GPU_SHARING:-1}"
SERIAL="emulator-$PORT"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"
LOGS="$ROOT/build-linux-emulator"
AVD_HOME="${ANDROID_AVD_HOME:-${ANDROID_USER_HOME:-$HOME/.android}/avd}"
REPOSITORY=https://dl.google.com/android/repository/
# What Refract is tested with: Digitalis is built against this system image's Android build, and emulator
# 37.1.11 is the build the Windows path runs. Checksums are the ones Google's repository lists, except the
# emulator's: Google no longer lists that build, so its SHA-1 was recorded when Refract first downloaded it.
# Fields: label | archive | SHA-1 | SDK folder | a file that shows it is installed.
PACKAGES=(
    "platform tools 37.0.1|platform-tools_r37.0.1-linux.zip|477254aa5f903c15cf51001717bdf347fb6b53e0|platform-tools|platform-tools/adb"
    "build tools 36.1.0|build-tools_r36.1_linux.zip|936a0d6bd5ae3e2118a7567dddbc95ff67ed46e9|build-tools/36.1.0|build-tools/36.1.0/aapt2"
    "Android 10 platform|platform-29_r05.zip|9d8a7e0ffa5168dbca6c60355b9129c6c7572aff|platforms/android-29|platforms/android-29/android.jar"
    "NDK r27d|android-ndk-r27d-linux.zip|22105e410cf29afcf163760cc95522b9fb981121|ndk/27.3.13750724|ndk/27.3.13750724/source.properties"
    "Android Emulator 37.1.11|emulator-linux_x64-15917651.zip|1b1f78891abf8ec268264356e1365c25519e8379|emulator|emulator/emulator"
    "Android 16 system image|sys-img/google_apis/x86_64-36_r07.zip|c6bf44bdcd885bb902b4ba752d111a073ad7a817|system-images/android-36/google_apis/x86_64|system-images/android-36/google_apis/x86_64/system.img"
)
JDK_URL=https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/OpenJDK21U-jdk_x64_linux_hotspot_21.0.12.1_1.tar.gz
JDK_SHA256=ce79869e1307ed8ee1e2baa86a412b1eb5b75d10a01006d788a6f968bcfaee94
# Refract's Android-side packages (android-runtime-apk/build_apk.sh, platform-sdk/build_apk.sh).
GUEST_APKS=("$ROOT/build-android-runtime-linux-arm64-v8a/refract-openxr-runtime-debug.apk"
            "$ROOT/build-android-runtime-linux-arm64-v8a/refract-systemdriver-debug.apk"
            "$ROOT/build-platform-sdk-linux/refract-platform-debug.apk")

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
adb_s() { "$ADB" -s "$SERIAL" "$@"; }
prop() { adb_s shell getprop "$1" 2>/dev/null | tr -d '\r'; }

download() {  # archive sha1 -> prints the local file
    local archive="$1" sha1="$2" folder="$SDK/.refract-downloads"
    local file="$folder/$(basename "$archive")"
    mkdir -p "$folder"
    if [[ ! -f "$file" ]]; then
        curl -fL --retry 4 --retry-delay 3 -C - -o "$file.part" "$REPOSITORY$archive" >&2
        mv "$file.part" "$file"
    fi
    if [[ "$(sha1sum "$file" | cut -d' ' -f1)" != "$sha1" ]]; then
        rm -f "$file"
        die "$(basename "$archive") was damaged in download; run setup again"
    fi
    echo "$file"
}

# Unpacks an archive whose single top folder becomes <sdk>/<dir>, replacing an older version there.
extract() {  # zip dir
    local staging="$SDK/.refract-downloads/unpack"
    rm -rf "$staging"
    mkdir -p "$staging"
    unzip -q "$1" -d "$staging"
    local entries=("$staging"/*)
    [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]] || die "$(basename "$1") has an unexpected layout"
    mkdir -p "$(dirname "$SDK/$2")"
    rm -rf "${SDK:?}/$2"
    mv "${entries[0]}" "$SDK/$2"
    rm -rf "$staging" "$1"
}

create_avd() {
    local dir="$AVD_HOME/$AVD.avd"
    [[ -f "$dir/config.ini" ]] && return 0
    mkdir -p "$dir"
    # As the launcher creates it (launcher/core/android_sdk.mjs): host GPU, shared-memory graphics transport
    # (hw.gltransport=asg), room for large games.
    cat >"$dir/config.ini" <<EOF
avd.ini.encoding=UTF-8
AvdId=$AVD
avd.ini.displayname=$AVD
PlayStore.enabled=no
abi.type=x86_64
hw.cpu.arch=x86_64
image.sysdir.1=system-images/android-36/google_apis/x86_64/
tag.id=google_apis
tag.display=Google APIs
target=android-36
hw.cpu.ncore=$CORES
hw.ramSize=$MEMORY_MB
vm.heapSize=228M
disk.dataPartition.size=32G
userdata.useQcow2=no
hw.useext4=yes
hw.gpu.enabled=yes
hw.gpu.mode=host
hw.gltransport=asg
hw.gltransport.asg.dataRingSize=32768
hw.gltransport.asg.writeBufferSize=1048576
hw.gltransport.asg.writeStepSize=4096
hw.gltransport.drawFlushInterval=800
hw.lcd.width=1080
hw.lcd.height=1920
hw.lcd.density=420
hw.keyboard=yes
hw.mainKeys=no
hw.audioInput=yes
hw.audioOutput=yes
hw.sdCard=no
hw.camera.back=none
hw.camera.front=none
fastboot.forceColdBoot=yes
showDeviceFrame=no
EOF
    printf 'avd.ini.encoding=UTF-8\npath=%s\ntarget=android-36\n' "$dir" >"$AVD_HOME/$AVD.ini"
    say "Created the AVD $AVD"
}

cmd_setup() {
    local entry label archive sha1 dir marker
    for entry in "${PACKAGES[@]}"; do
        IFS='|' read -r label archive sha1 dir marker <<<"$entry"
        if [[ -e "$SDK/$marker" ]]; then say "Have the $label"; continue; fi
        say "Downloading the $label"
        extract "$(download "$archive" "$sha1")" "$dir"
    done
    if [[ ! -x "$JDK/bin/javac" ]]; then
        say "Downloading a JDK (Temurin 21) for building the Android packages"
        local tarball="$SDK/.refract-downloads/jdk-21.tar.gz"
        curl -fL --retry 4 -o "$tarball" "$JDK_URL"
        [[ "$(sha256sum "$tarball" | cut -d' ' -f1)" == "$JDK_SHA256" ]] || { rm -f "$tarball"; die "the JDK was damaged in download"; }
        rm -rf "$JDK" && mkdir -p "$JDK"
        tar -xzf "$tarball" -C "$JDK" --strip-components=1
        rm -f "$tarball"
    fi
    create_avd
    say "Android is set up in $SDK (JDK: $JDK). Build Refract's packages with:"
    say "  JAVA_HOME=$JDK ANDROID_HOME=$SDK ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh"
    say "  JAVA_HOME=$JDK ANDROID_HOME=$SDK platform-sdk/build_apk.sh"
}

running() { "$ADB" devices 2>/dev/null | grep -q "^$SERIAL[[:space:]]*device"; }

wait_boot() {  # [pid] [minutes]
    local pid="${1:-}" deadline=$((SECONDS + 60 * ${2:-4}))
    while ((SECONDS < deadline)); do
        [[ "$(prop sys.boot_completed)" == 1 ]] && return 0
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
            die "the emulator stopped: $(grep -E '^(FATAL|ERROR)' "$LOGS/emulator.log" | tail -n1) (log: $LOGS/emulator.log)"
        fi
        sleep 2
    done
    die "Android did not finish starting (log: $LOGS/emulator.log)"
}

# adb root restarts adbd; wait until it is back as root.
adb_root() {
    adb_s root >/dev/null 2>&1 || true
    sleep 2
    adb_s wait-for-device
    [[ "$(adb_s shell id -u | tr -d '\r')" == 0 ]] || die "adb could not become root (a Google APIs image is needed, not Google Play)"
}

reboot_android() {
    adb_s reboot >/dev/null 2>&1 || true
    sleep 5
    adb_s wait-for-device
    wait_boot "${EMULATOR_PID:-}"
}

verify_gpu() {
    local gles
    gles="$(adb_s shell dumpsys SurfaceFlinger | tr -d '\r' | grep '^GLES:' || true)"
    [[ -n "$gles" ]] || die "cannot identify Android's GLES renderer"
    [[ ! "$gles" =~ SwiftShader|llvmpipe|softpipe|software ]] || die "Android is using software graphics ($gles)"
    mkdir -p "$LOGS"
    adb_s shell cmd gpu vkjson >"$LOGS/guest-vulkan.json"
    python3 -I - "$LOGS/guest-vulkan.json" <<'PY' || die "Android sees no hardware Vulkan GPU (see build-linux-emulator/guest-vulkan.json)"
import json, sys
devices = json.load(open(sys.argv[1])).get("devices", [])
ok = False
for device in devices:
    p = device["properties"]
    print(f"Vulkan: {p['deviceName']} (vendor {p['vendorID']}, type {p['deviceType']})")
    ok = ok or (p["deviceType"] != 4 and "SwiftShader" not in p["deviceName"])
sys.exit(0 if ok else 1)
PY
    say "$gles"
}

verify_abi() {
    local abis bridge
    abis="$(prop ro.product.cpu.abilist)"
    [[ ",$abis," == *,arm64-v8a,* ]] || die "Android does not offer arm64-v8a (ABIs: $abis)"
    bridge="$(prop ro.dalvik.vm.native.bridge)"
    say "ABIs: $abis; native bridge: $bridge"
}

# Digitalis, as scripts/translator.ps1 -Use digitalis -Refresh installs it: next to Google's translator on the
# writable /system, the active set copied into /system/lib64/arm64 and /system/bin/arm64.
digitalis_current() {
    [[ "$(prop ro.dalvik.vm.native.bridge)" == libberberis_arm64.so ]] || return 1
    local bundled installed
    bundled="$(sha256sum "$ROOT/prebuilts/digitalis/system/lib64/libberberis_arm64.so" | cut -d' ' -f1)"
    installed="$(adb_s shell sha256sum /system/lib64/libberberis_arm64.so 2>/dev/null | tr -d '\r' | cut -d' ' -f1)"
    [[ "$bundled" == "$installed" ]]
}

writable_system() {
    adb_root
    adb_s remount >/dev/null 2>&1 || true
    [[ "$(adb_s shell 'touch /system/.refract-rw && rm /system/.refract-rw && echo ok' | tr -d '\r')" == ok ]] && return 0
    # A new AVD's first remount only turns verity off; /system becomes writable after a reboot.
    say "Restarting Android to make /system writable"
    reboot_android
    adb_root
    adb_s remount >/dev/null 2>&1 || true
    [[ "$(adb_s shell 'touch /system/.refract-rw && rm /system/.refract-rw && echo ok' | tr -d '\r')" == ok ]] ||
        die "/system could not be made writable (the emulator must run with -writable-system)"
}

install_digitalis() {
    digitalis_current && return 0
    [[ "$(prop ro.build.version.sdk)" -ge 36 ]] || die "Digitalis needs an Android 16 (API 36) AVD"
    local bundle="$ROOT/prebuilts/digitalis/system"
    (cd "$ROOT/prebuilts/digitalis" && sha256sum --quiet -c SHA256SUMS) || die "the Digitalis bundle failed its checksums"
    say "Installing the Digitalis ARM translator (Android restarts)"
    writable_system
    adb_s shell '[ -d /system/lib64/arm64.google ] || { cp -a /system/lib64/arm64 /system/lib64/arm64.google && cp -a /system/bin/arm64 /system/bin/arm64.google && cp -a /system/etc/ld.config.arm64.txt /system/etc/ld.config.arm64.txt.google; }'
    # Unlink the old host libraries so the push makes new files instead of rewriting ones the zygote maps.
    adb_s shell 'rm -rf /system/lib64/arm64.digitalis /system/bin/arm64.digitalis /system/lib64/libberberis_*.so'
    adb_s push "$bundle/lib64/arm64" /system/lib64/arm64.digitalis >/dev/null 2>&1
    adb_s push "$bundle/bin/arm64" /system/bin/arm64.digitalis >/dev/null 2>&1
    adb_s push "$bundle/etc/ld.config.arm64.txt" /system/etc/ld.config.arm64.txt.digitalis >/dev/null 2>&1
    local file
    for file in "$bundle"/lib64/libberberis_*.so; do adb_s push "$file" "/system/lib64/$(basename "$file")" >/dev/null 2>&1; done
    for file in "$bundle"/bin/berberis_program_runner*; do adb_s push "$file" "/system/bin/$(basename "$file")" >/dev/null 2>&1; done
    adb_s shell 'chmod 644 /system/lib64/arm64.digitalis/* /system/lib64/libberberis_*.so /system/etc/ld.config.arm64.txt.digitalis; chmod 755 /system/bin/arm64.digitalis/* /system/bin/berberis_program_runner*'
    adb_s shell 'rm -rf /system/lib64/arm64 /system/bin/arm64 && cp -a /system/lib64/arm64.digitalis /system/lib64/arm64 && cp -a /system/bin/arm64.digitalis /system/bin/arm64 && cp /system/etc/ld.config.arm64.txt.digitalis /system/etc/ld.config.arm64.txt'
    adb_s shell "sed -i 's/^ro.dalvik.vm.native.bridge=.*/ro.dalvik.vm.native.bridge=libberberis_arm64.so/' /system/build.prop"
    adb_s shell 'restorecon -R /system/lib64/arm64 /system/bin/arm64 /system/etc/ld.config.arm64.txt /system/lib64 /system/bin 2>/dev/null; sync' || true
    reboot_android
    digitalis_current || die "Digitalis did not become Android's native bridge"
}

# Unmodified Quest games use the Meta Platform SDK (Refract's stand-in) only when Build.MANUFACTURER says Oculus,
# so the device presents itself as a Quest 3 (as the launcher's prepare() does).
install_identity() {
    [[ "$(prop ro.product.manufacturer)" =~ [Oo]culus ]] && return 0
    say "Setting up the Quest identity (Android restarts)"
    writable_system
    adb_s shell 'for f in /system/build.prop /vendor/build.prop /product/etc/build.prop /system_ext/etc/build.prop /odm/etc/build.prop; do [ -f $f ] || continue; [ -f $f.before-identity ] || cp -p $f $f.before-identity; sed -i -E "s/^(ro\.product\.([a-z_]+\.)?brand)=.*/\1=oculus/; s/^(ro\.product\.([a-z_]+\.)?manufacturer)=.*/\1=Oculus/; s/^(ro\.product\.([a-z_]+\.)?model)=.*/\1=Quest 3/" $f; done; sync'
    reboot_android
    [[ "$(prop ro.product.manufacturer)" =~ [Oo]culus ]] || die "Android did not take the Quest identity"
}

# Android sometimes boots without its zram swap; without it a big game thrashes (scripts note: Yeeps at ~0.1 fps).
ensure_swap() {
    local size=$((MEMORY_MB * 3 / 4))
    adb_root
    adb_s shell "grep -q zram0 /proc/swaps && exit 0; d=/system_dlkm/lib/modules; [ -e /sys/block/zram0 ] || { insmod \$d/zsmalloc.ko; insmod \$d/zram.ko; }; [ -e /sys/block/zram0 ] || exit 1; echo ${size}M > /sys/block/zram0/disksize && mkswap /dev/block/zram0 >/dev/null && swapon /dev/block/zram0" &&
        say "Android swap: on" || say "warning: could not turn on Android's zram swap; big games may stall"
}

install_guest_apks() {
    local apk installed local_hash remote
    for apk in "${GUEST_APKS[@]}"; do
        [[ -f "$apk" ]] || { say "warning: $(basename "$apk") is not built; skipped"; continue; }
        local package
        package="$("$SDK/build-tools/36.1.0/aapt2" dump packagename "$apk" 2>/dev/null || true)"
        installed="$({ adb_s shell pm path "$package" 2>/dev/null || true; } | tr -d '\r' | sed -n 's/^package:\(.*base\.apk\)$/\1/p' | head -n1)"
        local_hash="$(sha256sum "$apk" | cut -d' ' -f1)"
        remote="$([[ -n "$installed" ]] && adb_s shell sha256sum "$installed" | tr -d '\r' | cut -d' ' -f1 || true)"
        [[ "$remote" == "$local_hash" ]] && continue
        say "Installing $(basename "$apk")"
        adb_s install --no-incremental --force-queryable -r "$apk" | tail -n1
    done
}

cmd_start() {
    [[ -x "$EMULATOR" ]] || die "Android is not set up: run tools/linux_android_emulator.sh setup"
    [[ -r /dev/kvm && -w /dev/kvm ]] || die "this user cannot use /dev/kvm (add yourself to the kvm group)"
    if running; then
        say "$SERIAL is already running"
    else
        mkdir -p "$LOGS"
        # -writable-system: Digitalis and the Quest identity live on /system. -crash-report-mode never: after a
        # gfxstream crash the next start would otherwise wait on a consent prompt nobody sees.
        local -a args=(-avd "$AVD" -port "$PORT" -gpu host -accel on -no-snapshot -no-boot-anim -memory "$MEMORY_MB"
                       -cores "$CORES" -writable-system -crash-report-mode never -allow-host-audio)
        [[ "${REFRACT_SHOW_WINDOW:-0}" == 1 ]] || args+=(-no-window)
        # Must come last: everything after -qemu goes to qemu, whose -append the emulator adds to the kernel command line.
        [[ -z "${REFRACT_KERNEL_ARGS:-}" ]] || args+=(-qemu -append "$REFRACT_KERNEL_ARGS")
        local -a env=(ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK")
        # Refract's layer in the emulator's host Vulkan (build_host.sh builds it): it shares the game's eye images
        # with the viewer and host bridge, as on Windows, and repairs gfxstream descriptor templates that crash
        # Nvidia's driver (tools/windows_gpu_layer).
        local layer="$ROOT/build-linux/gpu-layer"
        if [[ -f "$layer/refract_gpu_layer.json" && -f "$layer/librefract_gpu_layer.so" ]]; then
            env+=(VK_ADD_LAYER_PATH="$layer" VK_INSTANCE_LAYERS=VK_LAYER_REFRACT_gpu_share)
        else
            say "warning: the Refract GPU layer is not built (./build_host.sh): frames are read back through the CPU"
            GPU_SHARING=0
        fi
        # The emulator's own PulseAudio client does not find PipeWire's Pulse socket by itself.
        local pulse="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native"
        [[ -n "${PULSE_SERVER:-}" || ! -S "$pulse" ]] || env+=(PULSE_SERVER="unix:$pulse")
        # On hybrid Intel CPUs Android's vCPUs stay on the performance cores, as tools/p_core_affinity.ps1 does on
        # Windows; E-cores are left to the viewer, the pose server and the desktop.
        local -a pin=()
        if [[ "${REFRACT_ANY_CORE:-0}" != 1 && -r /sys/devices/cpu_core/cpus && -r /sys/devices/cpu_atom/cpus ]]; then
            pin=(taskset -c "$(cat /sys/devices/cpu_core/cpus)")
        fi
        say "Starting Android ($AVD, $CORES cores, $MEMORY_MB MB${pin:+, P-cores $(cat /sys/devices/cpu_core/cpus)})"
        env "${env[@]}" nohup "${pin[@]}" "$EMULATOR" "${args[@]}" >"$LOGS/emulator.log" 2>&1 &
        EMULATOR_PID=$!
        "$ADB" start-server >/dev/null
        for _ in $(seq 60); do "$ADB" devices | grep -q "^$SERIAL" && break; kill -0 "$EMULATOR_PID" 2>/dev/null || break; sleep 1; done
        wait_boot "$EMULATOR_PID" 6
    fi
    verify_gpu
    install_digitalis
    install_identity
    verify_abi
    # A new Android's one-time "Viewing full screen" notice takes focus, so a Unity game pauses and never draws;
    # crash dialogs of system apps do the same. Nobody can tap them here.
    adb_s shell settings put secure immersive_mode_confirmations confirmed
    adb_s shell settings put global hide_error_dialogs 1
    ensure_swap
    install_guest_apks
    adb_s reverse tcp:38490 tcp:38490 >/dev/null
    adb_s reverse tcp:38491 tcp:38491 >/dev/null
    # The runtime then sends each frame as a 16-byte reference to the layer's shared textures (read once per game).
    if [[ "$GPU_SHARING" == 1 ]] && grep -q "Refract GPU layer: instance active" "$LOGS/emulator.log" 2>/dev/null; then
        adb_s shell setprop debug.refract.gpu_share 1
        say "GPU sharing: on"
    else
        adb_s shell setprop debug.refract.gpu_share 0
        say "GPU sharing: off (frames are read back through the CPU)"
    fi
    say "Ready: $SERIAL. Images use adb reverse :38491; poses use 10.0.2.2:38490."
}

case "${1:-}" in
    setup) cmd_setup ;;
    start) cmd_start ;;
    verify) verify_gpu; verify_abi ;;
    stop) adb_s emu kill ;;
    *) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
esac
