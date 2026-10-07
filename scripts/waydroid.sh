#!/usr/bin/env bash
# Runs Quest (Android OpenXR) games on Linux in Waydroid, the Linux counterpart of tools/run_windows_game.ps1.
# Android is the version the Windows emulator runs, Android 16, from WayDroid-ATV's LineageOS 23.2 images. Waydroid runs it in a container on this PC's own kernel: no virtual
# machine. On an Nvidia GPU, Android's Vulkan runs on Nvidia's own driver through waydroid-nvidia: Mesa's Venus
# driver sends the calls over a Unix socket to a renderer on the host, much as the emulator's Gfxstream does, and
# Refract's GPU layer in that renderer hands the game's eye images to the PC viewer without a copy through the CPU.
#
#   scripts/waydroid.sh check                 What is ready and what is missing (changes nothing).
#   sudo scripts/waydroid.sh setup [options]  One time: Waydroid, Android 16, the GPU stack, the ARM64 translator,
#                                             the Quest identity and network access.
#   scripts/waydroid.sh install [APK...]      Installs Refract's Android packages, and game APKs if given.
#   scripts/waydroid.sh play [PACKAGE] [options]
#                                             Plays a game (default: North Star) in a window on this PC, like
#                                             run_windows_game.ps1 -PcViewer. Closing the window or Ctrl+C stops it.
#
# setup options:
#   --gpu /dev/dri/renderDN   The GPU Android draws with. Default: the one driving the desktop. Nvidia's driver
#                             runs through Venus; AMD, Intel and nouveau through Waydroid's Mesa.
#   --images DIR              Use system.img and vendor.img from DIR instead of downloading Android 16.
#   --translator NAME         The ARM64 translator: ndk (default: the image's own ndk_translation), digitalis
#                             (the Windows emulator's, built for this image's Android 16 QPR2 in
#                             prebuilts/digitalis-qpr2: tools/translator/digitalis_build_qpr2.sh) or none.
#   --no-translator           Same as --translator none (only x86_64 apps then run).
#   --no-identity             Keep Waydroid's device identity instead of reporting a Quest 3.
# play options:
#   --activity NAME           The activity to start (default: the app's launcher or Meta VR activity).
#   --name TEXT               The window title (default: the app's name from its APK).
#   --owned                   Report the game as owned to the Meta Platform stand-in (as owned_games.txt does).
#   --eye-size PIXELS         Square render size per eye in the PC window (512-4096, default 1600).
#   --headset                 Play on a headset through the PC OpenXR runtime (the host bridge) instead.
#   --render-scale PERCENT    Headset only: scales the headset's recommended eye size (25-200, default 100).
#   --runtime FILE.json       Headset only: the PC OpenXR runtime manifest (sets XR_RUNTIME_JSON).
#   --run NAME                Log directory under runs/ (default: waydroid-<time>).
#   --no-unreal-policy        Restore an Unreal game's own texture pool (as -UnrealMemoryPolicy Off on Windows).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WAYDROID_WORK=/var/lib/waydroid
CFG="$WAYDROID_WORK/waydroid.cfg"
OVERLAY="$WAYDROID_WORK/overlay"
LXC_NODES="$WAYDROID_WORK/lxc/waydroid/config_nodes"
HOST_BRIDGE="${REFRACT_HOST_BRIDGE:-$ROOT/build-linux/host-bridge/refract-host-bridge}"
VIEWER="$ROOT/build-linux/refract-viewer"
GPU_LAYER_DIR="$ROOT/build-linux/gpu-layer"
RUNTIME_APKS=("$ROOT/build-android-runtime-linux-arm64-v8a/refract-openxr-runtime-debug.apk"
              "$ROOT/build-android-runtime-linux-arm64-v8a/refract-systemdriver-debug.apk"
              "$ROOT/build-platform-sdk-linux/refract-platform-debug.apk")
RUNTIME_PACKAGE=com.refract.openxrruntime
DEFAULT_PACKAGE=com.meta.samples.NorthStar
BRIDGE_PORTS=(38490 38491)
# Waydroid's DRM drivers whose Android userspace (Mesa) exists; see waydroid tools/helpers/gpu.py.
MESA_DRIVERS="i915 xe amdgpu radeon nouveau virtio_gpu panfrost msm vc4"

# Android 16 for Waydroid: WayDroid-ATV's LineageOS 23.2 (Android 16 QPR2). Waydroid's own images are Android 13.
# Kept in Waydroid's preinstalled-image directory, so its updater never swaps them for its own.
ANDROID_URL=https://github.com/WayDroid-ATV/waydroid-builds/releases/download/20260717
ANDROID_ZIPS=("lineage-23.2-20260717-VANILLA-waydroid_x86_64-system.zip 152e0f5c01cf10cc4c7ec93723c286308a06cb1334e6eeb4a4039cb345f97ae4"
              "lineage-23.2-20260717-MAINLINE-waydroid_x86_64-vendor.zip 28dfbd0e423dff6d5c7d2e34e15011fbaf4865da769d9923b84656d87e9f4d3c")
IMAGES=/etc/waydroid-extra/images
IMAGES_STAMP="$IMAGES/refract-release"
ANDROID_RELEASE=lineage-23.2-20260717

# waydroid-nvidia (github.com/Shiro836/waydroid-nvidia): the host renderer, and Android's Venus Vulkan driver,
# gralloc backend and ANGLE. Its hwcomposer and surfaceflinger are LineageOS 20 builds and are left out: Android 16
# uses the image's own.
VENUS_VERSION=v0.1.2
VENUS_URL=https://github.com/Shiro836/waydroid-nvidia/releases/download/$VENUS_VERSION
VENUS_TARBALLS=("waydroid-nvidia-host-x86_64-$VENUS_VERSION.tar.zst 6372d4f78ff4e9442e32a60d9b1ead11f238124f5acd57a92bbb4fd74c9d61e8"
                "waydroid-nvidia-guest-android-x86_64-$VENUS_VERSION.tar.zst c0a6ee7a69c6bc6075f7197d6c3cc2e213bf1b061b032f20da6f7441f8704574"
                "waydroid-nvidia-guest-prebuilts-$VENUS_VERSION.tar.zst 61899f56c203b750d41f7c141a1ee264cb68241748cc9cda512ed45f2997cbd1")
VENUS_DIR="/usr/local/lib/refract/waydroid-nvidia-$VENUS_VERSION"
VENUS_GUEST_LIBS=(vendor/lib/hw/vulkan.virtio.so vendor/lib64/hw/vulkan.virtio.so vendor/lib64/libgbm_mesa_wrapper.so
                  vendor/lib/egl/libEGL_angle.so vendor/lib/egl/libGLESv1_CM_angle.so vendor/lib/egl/libGLESv2_angle.so
                  vendor/lib64/egl/libEGL_angle.so vendor/lib64/egl/libGLESv1_CM_angle.so vendor/lib64/egl/libGLESv2_angle.so)
# The renderer's socket; the container sees this directory as /dev/venus.
VENUS_SOCKET_DIR=/run/refract-venus
VENUS_UNIT=refract-venus.service

say() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

adb_bin() {
    if [[ -n "${ADB:-}" ]]; then echo "$ADB"
    elif have adb; then command -v adb
    else echo "${ANDROID_HOME:-$HOME/Android/Sdk}/platform-tools/adb"; fi
}
ADB="$(adb_bin)"

waydroid_status() { waydroid status 2>/dev/null || true; }
waydroid_field() { waydroid_status | sed -n "s/^$1:[[:space:]]*//p" | head -n1 || true; }
session_running() { [[ "$(waydroid_field Session)" == RUNNING ]]; }
# The host's address on Waydroid's network bridge, which Android reaches the host bridge at.
host_address() { ip -4 -o addr show dev waydroid0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1 || true; }
render_driver() { basename "$(readlink -f "/sys/class/drm/$(basename "$1")/device/driver" 2>/dev/null)" 2>/dev/null || true; }
# vendor:device of a render node's GPU, as Mesa's device-select layer names it (10de:2216).
render_pci_id() {
    local device="/sys/class/drm/$(basename "$1")/device"
    printf '%s:%s\n' "$(sed 's/^0x//' "$device/vendor")" "$(sed 's/^0x//' "$device/device")"
}
render_has_display() {
    local status
    for status in /sys/class/drm/"$(basename "$1")"/device/drm/card*/card*-*/status; do
        [[ -r "$status" && "$(<"$status")" == connected ]] && return 0
    done
    return 1
}
# The GPU driving the desktop, else the first one Android has a driver for, else the first Nvidia one.
default_gpu() {
    local node
    for node in /dev/dri/renderD*; do [[ -e "$node" ]] && render_has_display "$node" && { echo "$node"; return; }; done
    for node in /dev/dri/renderD*; do [[ " $MESA_DRIVERS " == *" $(render_driver "$node") "* ]] && { echo "$node"; return; }; done
    for node in /dev/dri/renderD*; do [[ "$(render_driver "$node")" == nvidia ]] && { echo "$node"; return; }; done
}
config_get() {  # section key
    [[ -r "$CFG" ]] || return 0
    python3 -I - "$CFG" "$1" "$2" <<'PY'
import configparser, sys
cfg = configparser.ConfigParser(interpolation=None)
cfg.optionxform = str
cfg.read(sys.argv[1])
print(cfg.get(sys.argv[2], sys.argv[3], fallback=""))
PY
}
venus_mode() { [[ -n "$(config_get properties mesa.vtest.socket.name)" ]]; }
# The container mounts setup adds for Venus (the GPU's render node and the renderer's socket directory).
venus_mounts() {
    local gpu="$1"
    printf 'lxc.mount.entry = %s %s none bind,create=file,optional 0 0\n' "$gpu" "${gpu#/}"
    printf 'lxc.mount.entry = %s dev/venus none bind,create=dir,optional 0 0\n' "$VENUS_SOCKET_DIR"
}

SERIAL="" USER_HOME="$HOME"
adb_s() { "$ADB" -s "$SERIAL" "$@"; }
# Waydroid freezes the whole container when Android's screen goes off (nothing shows it on this PC), and a frozen
# adbd accepts connections but never answers. Its container service thaws it for the session's user.
unfreeze() {
    [[ "$(waydroid_field Container)" == FROZEN ]] || return 0
    python3 -c 'import dbus; dbus.SystemBus().get_object("id.waydro.Container", "/ContainerManager").Unfreeze(dbus_interface="id.waydro.ContainerManager")' ||
        die "Waydroid's container is frozen and could not be thawed"
}
# Connects adb to the running container and waits until Android has booted.
connect_adb() {
    [[ -x "$ADB" ]] || die "adb not found. Install android-tools (Fedora) or adb (Debian/Ubuntu), or set ADB=/path/to/adb."
    unfreeze
    local ip=""
    for _ in $(seq 60); do
        ip="$(waydroid_field 'IP address')"
        [[ "$ip" =~ ^[0-9.]+$ ]] && break  # UNKNOWN until Android's network is up.
        sleep 1
    done
    [[ "$ip" =~ ^[0-9.]+$ ]] || die "Waydroid reports no IP address; is its session running?"
    SERIAL="$ip:5555"
    "$ADB" disconnect "$SERIAL" >/dev/null 2>&1 || true
    # adbd starts late in Android's boot, and adb does not retry a TCP connection that failed: keep asking.
    local state=""
    for _ in $(seq 90); do
        "$ADB" connect "$SERIAL" >/dev/null 2>&1 || true
        state="$("$ADB" -s "$SERIAL" get-state 2>/dev/null || true)"
        [[ "$state" == device ]] && break
        [[ "$state" == offline ]] && "$ADB" disconnect "$SERIAL" >/dev/null 2>&1
        sleep 2
    done
    [[ "$state" == device ]] ||
        die "adb could not reach Android at $SERIAL (adb is set up by 'sudo scripts/waydroid.sh setup'; if Android asks, allow this PC)"
    for _ in $(seq 120); do
        [[ "$(adb_s shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] && break
        sleep 1
    done
    [[ "$(prop sys.boot_completed)" == 1 ]] || die "Android did not finish booting"
    # Keeps the screen on, so Waydroid never freezes Android under a running game.
    adb_s shell 'svc power stayon true; settings put system screen_off_timeout 2147483647; input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1 || true
}
prop() { adb_s shell getprop "$1" 2>/dev/null | tr -d '\r'; }

cmd_check() {
    local problems=0
    problem() { say "  [missing] $*"; problems=$((problems + 1)); }
    ok() { say "  [ok] $*"; }

    say "Linux host"
    if grep -qw binder /proc/filesystems 2>/dev/null || [[ -e /dev/binder ]] ||
       grep -q '^CONFIG_ANDROID_BINDERFS=y' "/boot/config-$(uname -r)" 2>/dev/null; then
        ok "kernel has Android binder"
    else
        problem "kernel without Android binder (binderfs): Waydroid cannot run"
    fi
    local node driver
    for node in /dev/dri/renderD*; do
        [[ -e "$node" ]] || continue
        driver="$(render_driver "$node")"
        local display=""
        render_has_display "$node" && display=", drives the desktop"
        if [[ "$driver" == nvidia ]]; then
            say "  [--] $node (nvidia $(render_pci_id "$node")$display): Android can use it through Venus"
        elif [[ " $MESA_DRIVERS " == *" $driver "* ]]; then
            say "  [--] $node ($driver$display): Android can use it through Mesa"
        else
            say "  [--] $node ($driver$display): no Android driver"
        fi
    done
    [[ -x "$VIEWER" ]] && ok "PC viewer built" || problem "PC viewer not built: run ./build_host.sh"
    [[ -f "$GPU_LAYER_DIR/librefract_gpu_layer.so" ]] && ok "GPU sharing layer built" ||
        problem "GPU sharing layer not built: run ./build_host.sh (the viewer then gets pixels through the CPU)"
    local apk
    for apk in "${RUNTIME_APKS[@]}"; do
        [[ -f "$apk" ]] || problem "$(basename "$apk") not built (ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh, platform-sdk/build_apk.sh)"
    done

    say "Waydroid"
    if ! have waydroid; then
        problem "waydroid is not installed: sudo scripts/waydroid.sh setup"
        say "$problems problem(s)."
        return 1
    fi
    ok "$(waydroid --version 2>/dev/null | head -n1 || echo waydroid) installed"
    if [[ ! -r "$CFG" ]]; then
        problem "Waydroid is not initialised: sudo scripts/waydroid.sh setup"
        say "$problems problem(s)."
        return 1
    fi
    [[ "$(cat "$IMAGES_STAMP" 2>/dev/null)" == "$ANDROID_RELEASE" ]] && ok "Android 16 images ($ANDROID_RELEASE)" ||
        problem "Waydroid does not use Refract's Android 16 images: sudo scripts/waydroid.sh setup"
    local bridge
    bridge="$(config_get properties ro.dalvik.vm.native.bridge)"
    case "$bridge" in
        libndk_translation.so) ok "ARM64 translator: ndk_translation" ;;
        libberberis_arm64.so) ok "ARM64 translator: Digitalis" ;;
        *) problem "no ARM64 translator set up: sudo scripts/waydroid.sh setup" ;;
    esac
    if venus_mode; then
        local gpu entry
        gpu="$(config_get properties gralloc.gbm.device)"
        ok "Android draws through Venus on $gpu ($(render_pci_id "$gpu"))"
        while IFS= read -r entry; do
            grep -qxF "$entry" "$LXC_NODES" 2>/dev/null ||
                problem "container mount missing (a 'waydroid upgrade' drops it): sudo scripts/waydroid.sh setup"
        done < <(venus_mounts "$gpu")
        [[ -x "$VENUS_DIR/host/virgl_test_server" ]] || problem "Venus renderer missing: sudo scripts/waydroid.sh setup"
        if systemctl --user is-active --quiet "$VENUS_UNIT"; then ok "Venus renderer running ($VENUS_UNIT)"
        else problem "Venus renderer not running: systemctl --user enable --now $VENUS_UNIT"; fi
    fi
    if ! session_running; then
        say "  [--] the Waydroid session is not running; start it ('waydroid session start') for the Android checks."
        say "$problems problem(s) found so far."
        return $((problems > 0))
    fi
    if [[ ! -x "$ADB" ]]; then
        problem "adb not found (Fedora: sudo dnf install android-tools; or set ADB=/path/to/adb)"
        say "$problems problem(s) found so far."
        return 1
    fi
    connect_adb
    local sdk vulkan
    sdk="$(prop ro.build.version.sdk)"
    if [[ "$sdk" -ge 36 ]]; then ok "Android API $sdk"; else problem "Android API $sdk: Refract needs Android 16 (API 36)"; fi
    [[ "$(prop ro.dalvik.vm.native.bridge)" == "$bridge" ]] && ok "native bridge active ($(prop ro.product.cpu.abilist))" ||
        problem "native bridge is '$(prop ro.dalvik.vm.native.bridge)', not '$bridge': restart Waydroid after setup"
    vulkan="$(prop ro.hardware.vulkan)"
    if [[ -n "$vulkan" && "$vulkan" != pastel ]]; then ok "Android Vulkan driver: $vulkan"; else problem "Android has no hardware Vulkan (ro.hardware.vulkan='$vulkan')"; fi
    local gles
    gles="$(adb_s shell dumpsys SurfaceFlinger 2>/dev/null | tr -d '\r' | sed -n 's/^GLES: //p' | head -n1 || true)"
    say "  [--] GPU: ${gles:-unknown}"
    say "  [--] device identity: $(prop ro.product.manufacturer) $(prop ro.product.model)"
    adb_s shell pm path "$RUNTIME_PACKAGE" >/dev/null 2>&1 && ok "Refract runtime installed" ||
        problem "Refract runtime not installed: scripts/waydroid.sh install"
    local host
    host="$(host_address)"
    [[ -n "$host" ]] && ok "Android reaches this PC at $host" || problem "no waydroid0 network bridge"
    say "$problems problem(s)."
    return $((problems > 0))
}

# Downloads URL to FILE unless FILE, or a copy in the user's ~/.cache/refract/downloads, already has the
# SHA-256; fails on a mismatch.
fetch() {  # url file sha256
    if [[ -f "$2" ]] && sha256sum "$2" | grep -q "^$3 "; then return 0; fi
    local cached="$USER_HOME/.cache/refract/downloads/$(basename "$2")"
    if [[ -f "$cached" ]] && sha256sum "$cached" | grep -q "^$3 "; then cp "$cached" "$2"; return 0; fi
    say "Downloading $(basename "$2")"
    curl -fL --retry 3 -o "$2.part" "$1" || die "download failed: $1"
    sha256sum "$2.part" | grep -q "^$3 " || { rm -f "$2.part"; die "$(basename "$2") does not have the expected SHA-256"; }
    mv "$2.part" "$2"
}

setup_android_images() {  # [dir with system.img and vendor.img]
    if [[ -n "${1:-}" ]]; then
        [[ -f "$1/system.img" && -f "$1/vendor.img" ]] || die "$1 needs system.img and vendor.img"
        install -d "$IMAGES"
        install -m 644 "$1/system.img" "$1/vendor.img" "$IMAGES/"
        echo "custom" >"$IMAGES_STAMP"
    elif [[ "$(cat "$IMAGES_STAMP" 2>/dev/null)" != "$ANDROID_RELEASE" ]]; then
        have unzip || die "unzip is needed"
        local cache=/var/cache/refract/waydroid-images entry
        install -d "$cache" "$IMAGES"
        for entry in "${ANDROID_ZIPS[@]}"; do
            fetch "$ANDROID_URL/${entry%% *}" "$cache/${entry%% *}" "${entry##* }"
            unzip -o -q "$cache/${entry%% *}" -d "$IMAGES" '*.img'
        done
        echo "$ANDROID_RELEASE" >"$IMAGES_STAMP"
        rm -rf "$cache"
    else
        return 0
    fi
    # -f re-initialises an existing install; the user's Android data (~/.local/share/waydroid) is kept.
    say "Initialising Waydroid with Android 16"
    systemctl stop waydroid-container 2>/dev/null || true
    waydroid init -f -i "$IMAGES"
}

setup_venus() {  # render node, user, user's home
    local gpu="$1" user="$2" home="$3"
    [[ "$(cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null)" == Y ]] ||
        die "Venus needs nvidia-drm.modeset=1 (Nvidia's driver shares no dma-bufs without it). Add nvidia-drm.modeset=1 to the kernel command line and reboot."
    local cache=/var/cache/refract/waydroid-nvidia entry name
    install -d "$cache" "$VENUS_DIR/host" "$VENUS_DIR/guest"
    for entry in "${VENUS_TARBALLS[@]}"; do
        name="${entry%% *}"
        fetch "$VENUS_URL/$name" "$cache/$name" "${entry##* }"
        case "$name" in
            *-host-*) tar --zstd -xf "$cache/$name" -C "$VENUS_DIR/host" --strip-components=1 ;;
            *) tar --zstd -xf "$cache/$name" -C "$VENUS_DIR/guest" --strip-components=1 ;;
        esac
    done
    rm -rf "$cache"
    # Android's Vulkan driver (Venus), gralloc backend and GL (ANGLE on Venus) go into the vendor overlay.
    local file
    for file in "${VENUS_GUEST_LIBS[@]}"; do
        [[ -f "$VENUS_DIR/guest/$file" ]] || die "waydroid-nvidia $VENUS_VERSION lacks $file"
        install -D -m 644 "$VENUS_DIR/guest/$file" "$OVERLAY/$file"
    done
    say "Installed waydroid-nvidia $VENUS_VERSION (Venus) into $OVERLAY/vendor"

    # The renderer runs as the desktop user (it needs the user's Nvidia access) and owns the socket's directory,
    # so it can always replace a stale socket; it makes the socket itself world-connectable for Android.
    echo "d $VENUS_SOCKET_DIR 0755 $user $(id -gn "$user") -" >/etc/tmpfiles.d/refract-venus.conf
    rm -f "$VENUS_SOCKET_DIR/venus.sock"
    systemd-tmpfiles --create /etc/tmpfiles.d/refract-venus.conf
    # Only Nvidia's Vulkan driver, with the chosen GPU listed first (Mesa's device-select layer; its force mode,
    # which would hide the others, breaks Venus). Refract's GPU layer (if built) shares the game's eye images with
    # the PC viewer; VK_LOADER_LAYERS_ENABLE skips it quietly when it is missing.
    local icd="" candidate
    for candidate in /usr/share/vulkan/icd.d/nvidia_icd*.json /etc/vulkan/icd.d/nvidia_icd*.json; do
        [[ -f "$candidate" ]] && { icd="$candidate"; break; }
    done
    [[ -n "$icd" ]] || die "Nvidia's Vulkan driver (nvidia_icd.json) is not installed"
    cat >/etc/systemd/user/$VENUS_UNIT <<EOF
[Unit]
Description=Venus renderer for Refract's Waydroid (Android Vulkan on the Nvidia driver)
StartLimitIntervalSec=0

[Service]
# The server does not replace a socket left behind by a previous one.
ExecStartPre=-/usr/bin/rm -f $VENUS_SOCKET_DIR/venus.sock
Environment="RENDER_SERVER_EXEC_PATH=$VENUS_DIR/host/virgl_render_server"
Environment="LD_LIBRARY_PATH=$VENUS_DIR/host"
Environment="VK_DRIVER_FILES=$icd"
Environment="MESA_VK_DEVICE_SELECT=$(render_pci_id "$gpu")"
Environment="VK_ADD_LAYER_PATH=$GPU_LAYER_DIR"
Environment=VK_LOADER_LAYERS_ENABLE=VK_LAYER_REFRACT_gpu_share
ExecStart="$VENUS_DIR/host/virgl_test_server" --venus --multi-clients --socket-path $VENUS_SOCKET_DIR/venus.sock
Restart=always
RestartSec=1

[Install]
WantedBy=default.target
EOF
    systemctl --global enable "$VENUS_UNIT" >/dev/null 2>&1 || true
    if [[ -n "$user" && "$user" != root ]]; then
        systemctl --user -M "$user@" daemon-reload 2>/dev/null &&
            systemctl --user -M "$user@" restart "$VENUS_UNIT" 2>/dev/null &&
            say "Started the Venus renderer for $user ($VENUS_UNIT)" ||
            warn "start the Venus renderer as $user: systemctl --user daemon-reload && systemctl --user enable --now $VENUS_UNIT"
    fi
}

# ATV's vendor init (waydroid-init) picks Android's graphics stack from /data/misc/waydroid_settings, and
# software rendering for an Nvidia GPU without it. Written into the user's Waydroid data, which is Android's /data.
write_waydroid_settings() {  # user, user's home, settings...
    local user="$1" home="$2"
    shift 2
    local data="$home/.local/share/waydroid/data" group
    group="$(id -gn "$user")"
    # Created as Waydroid would (by the user) if this is the first run; Android owns what is inside.
    local dir
    for dir in "$home/.local/share/waydroid" "$data"; do
        [[ -d "$dir" ]] || install -d -o "$user" -g "$group" "$dir"
    done
    # Android's own owner and mode for /data/misc (system, misc).
    [[ -d "$data/misc" ]] || install -d -o 1000 -g 9998 -m 1771 "$data/misc"
    printf '%s\n' "$@" >"$data/misc/waydroid_settings"
    chmod 644 "$data/misc/waydroid_settings"
}

# Fixes for Meta's XR Audio SDK under ndk_translation, as copies of the image's guest (ARM64) libraries in Waydroid's
# overlay. Only the known image's libraries are patched (by SHA-256); others are left alone.
#
# libandroid_runtime.so: ndk_translation has no bridge for its JNI_GetCreatedJavaVMs, and a call aborts the app
#   ("Bad 'JNI_GetCreatedJavaVMs' call"). The SDK looks the function up by name from its metrics code, so Quest games
#   crash at start. libnativehelper.so's bridge works, so this copy no longer exports it (its own GOT entry becomes a
#   relative relocation, and its .dynstr name changes): a lookup finds libnativehelper's, or nothing, which the SDK
#   handles.
# libc.so: the SDK's android_namespace_loader::LoadNamespace (in libMetaXRAudioWwise.so) needs libc's unexported
#   android_get_exported_namespace. It reads the .symtab distance from android_get_device_api_level in the libc file
#   on disk, which here is the x86_64 host libc (0x74960), and adds it to dlsym's address of the ARM64 one: the call
#   lands in unrelated code (Batman: Arkham Shadow's audio thread crashes at start). Digitalis redirects that file read
#   (tools/translator/digitalis-syslib-redirect.patch); ndk_translation cannot be changed, so this copy exports
#   android_get_device_api_level at a padding slot P that branches to the real one, and P + 0x74960, also padding,
#   branches to the ARM64 android_get_exported_namespace.
NDK_SHIMS=("system/lib64/arm64/libandroid_runtime.so cfe7cb44a9620d1d3008176b10fd8ac8301735b06f38989e71b3ed79ff6cf046 a3ed281083a4691d021a76260b3a1ac4eb923d30ef188be575617d0899c78114"
           "system/lib64/arm64/libc.so 6803ed4085de78b9a84c0c545b53972eda4c50a0f8cf4ab2ac4de057231d551a 610a4a00aa6d970551c34f976b95bfd8e66b76e4f90a937d6723c5e0be39f9d4")
setup_ndk_shims() {  # install | remove
    local entry lib original patched target source mnt="" status error
    for entry in "${NDK_SHIMS[@]}"; do
        read -r lib original patched <<<"$entry"
        target="$OVERLAY/$lib"
        if [[ -f "$target" && "$(sha256sum <"$target" | cut -d' ' -f1)" == "$patched" ]]; then
            [[ "$1" == remove ]] && rm -f "$target"
            continue
        fi
        [[ "$1" == remove ]] && continue
        # The original: from Waydroid's own mount of the image (no copy in the overlay hides it now), else the image.
        rm -f "$target"
        source="$WAYDROID_WORK/rootfs/$lib"
        if [[ ! -f "$source" ]]; then
            if [[ -z "$mnt" ]]; then
                mnt="$(mktemp -d)"
                if ! error="$(mount -t erofs -o ro,loop "$IMAGES/system.img" "$mnt" 2>&1)"; then
                    rmdir "$mnt"
                    warn "could not read $IMAGES/system.img ($error); Quest games using Meta's audio SDK may crash at start"
                    return 0
                fi
            fi
            source="$mnt/$lib"
        fi
        status=0
        python3 -I - "$source" "$target.new" "$original" "$patched" <<'PATCH' || status=$?
import hashlib, os, struct, sys
source, target, expected, patched = sys.argv[1:5]
d = bytearray(open(source, "rb").read())
if hashlib.sha256(d).hexdigest() != expected:
    sys.exit(3)
shoff, = struct.unpack_from("<Q", d, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
sections = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
names = sections[shstrndx][4]
def section(name):
    for s in sections:
        o = names + s[0]
        if d[o:d.index(b"\0", o)] == name.encode():
            return s
def dynsym_entry(symbol):
    dynsym, dynstr = section(".dynsym"), section(".dynstr")
    found = [o for o in range(dynsym[4], dynsym[4] + dynsym[5], 24)
             if d[dynstr[4] + struct.unpack_from("<I", d, o)[0]:].split(b"\0", 1)[0] == symbol]
    assert len(found) == 1
    return found[0]
if os.path.basename(source) == "libandroid_runtime.so":
    GOT_ENTRY, FUNCTION = 0x722A8, 0x41B00  # The export's GLOB_DAT slot and address in this build.
    rela = section(".rela.dyn")
    relocs = [o for o in range(rela[4], rela[4] + rela[5], 24) if struct.unpack_from("<Q", d, o)[0] == GOT_ENTRY]
    assert len(relocs) == 1 and struct.unpack_from("<Q", d, relocs[0] + 8)[0] & 0xFFFFFFFF == 1025  # GLOB_DAT
    struct.pack_into("<QQq", d, relocs[0], GOT_ENTRY, 1027, FUNCTION)  # R_AARCH64_RELATIVE
    dynstr = section(".dynstr")
    name = d.index(b"\0JNI_GetCreatedJavaVMs\0", dynstr[4], dynstr[4] + dynstr[5]) + 1
    d[name:name + 3] = b"jni"
else:  # libc.so
    API, NAMESPACE, P, Q = 0x68850, 0xDC0F4, 0x581E8, 0xCCB48  # Q - P: the host libc's distance, 0x74960.
    text = section(".text")
    def at(va): return va - text[3] + text[4]
    def branch(where, to): return struct.pack("<I", 0x14000000 | (((to - where) // 4) & 0x3FFFFFF))
    for slot in (P, Q):
        assert d[at(slot):at(slot) + 4] == b"\0\0\0\0"
    d[at(P):at(P) + 4] = branch(P, API)
    d[at(Q):at(Q) + 4] = branch(Q, NAMESPACE)
    entry = dynsym_entry(b"android_get_device_api_level")
    assert struct.unpack_from("<Q", d, entry + 8)[0] == API
    struct.pack_into("<Q", d, entry + 8, P)
assert hashlib.sha256(d).hexdigest() == patched
os.makedirs(os.path.dirname(target), exist_ok=True)
open(target, "wb").write(d)
PATCH
        case "$status" in
            0) mv -f "$target.new" "$target"
               chmod 644 "$target"
               say "Patched $(basename "$lib") for ndk_translation (Meta XR Audio SDK)" ;;
            3) warn "unknown $lib in this image; left as is (Quest games using Meta's audio SDK may crash)" ;;
            *) rm -f "$target.new"
               warn "could not patch $lib" ;;
        esac
    done
    if [[ -n "$mnt" ]]; then
        umount "$mnt"
        rmdir "$mnt"
    fi
}

# Lets this user's adb in without a prompt that nothing on this PC shows: their key goes into Android's list.
trust_adb_key() {  # user, user's home
    local user="$1" home="$2" key="$2/.android/adbkey.pub" keys="$2/.local/share/waydroid/data/misc/adb"
    local adb="$ADB"
    [[ -x "$adb" ]] || adb="${ANDROID_HOME:-$home/Android/Sdk}/platform-tools/adb"  # Under sudo, HOME is root's.
    if [[ ! -r "$key" && -x "$adb" ]]; then
        sudo -u "$user" "$adb" start-server >/dev/null 2>&1 || true  # Creates the user's key.
    fi
    [[ -r "$key" ]] || { warn "no adb key at $key yet: after installing adb, run this setup again"; return 0; }
    # Android's own owner and mode for /data/misc/adb (system, shell).
    [[ -d "$keys" ]] || install -d -o 1000 -g 2000 -m 2750 "$keys"
    if ! grep -qxF "$(cat "$key")" "$keys/adb_keys" 2>/dev/null; then
        cat "$key" >>"$keys/adb_keys"
        echo >>"$keys/adb_keys"
    fi
    chown 1000:2000 "$keys/adb_keys"
    chmod 640 "$keys/adb_keys"
}

cmd_setup() {
    local gpu="" images="" translator=ndk identity=1
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gpu) gpu="${2:?--gpu needs a render node}"; shift 2 ;;
            --images) images="${2:?--images needs a directory}"; shift 2 ;;
            --translator) translator="${2:?--translator needs ndk, digitalis or none}"; shift 2 ;;
            --no-translator) translator=none; shift ;;
            --no-identity) identity=0; shift ;;
            *) die "unknown setup option: $1" ;;
        esac
    done
    [[ " ndk digitalis none " == *" $translator "* ]] || die "--translator is ndk, digitalis or none, not '$translator'"
    [[ $EUID -eq 0 ]] || die "setup changes Waydroid's system files: run it with sudo"
    local user="${SUDO_USER:-}" home=""
    [[ -n "$user" && "$user" != root ]] || die "run setup with sudo from your desktop account (Waydroid's data is per user)"
    home="$(getent passwd "$user" | cut -d: -f6)"
    USER_HOME="$home"
    if ! have waydroid; then
        have dnf || die "install Waydroid 1.6.3 or newer first (https://docs.waydro.id/usage/install-on-desktops)"
        say "Installing Waydroid"
        dnf install -y waydroid
    fi

    [[ -n "$gpu" ]] || gpu="$(default_gpu)"
    [[ -n "$gpu" && -e "$gpu" ]] || die "no GPU render node found${gpu:+ ($gpu does not exist)}"
    local driver venus=0
    driver="$(render_driver "$gpu")"
    if [[ "$driver" == nvidia ]]; then venus=1
    elif [[ " $MESA_DRIVERS " != *" $driver "* ]]; then die "$gpu is driven by '$driver', which Android has no driver for"; fi
    say "Android will draw on $gpu ($driver $(render_pci_id "$gpu")$([[ $venus == 1 ]] && echo ', through Venus'))"

    setup_android_images "$images"
    [[ -r "$CFG" ]] || die "Waydroid did not initialise"

    local -a waydroid_settings=(mount_overlays=True) properties=() settings=()
    if [[ $venus == 1 ]]; then
        setup_venus "$gpu" "$user" "$home"
        # Waydroid skips Nvidia render nodes (drm_device stays unset); the mounts below add it.
        settings+=(ro.hardware.gralloc=minigbm_gbm_mesa ro.hardware.egl=angle ro.hardware.vulkan=virtio
                   "gralloc.gbm.device=$gpu")
        properties+=(ro.hardware.gralloc=minigbm_gbm_mesa ro.hardware.egl=angle ro.hardware.vulkan=virtio
                     "gralloc.gbm.device=$gpu"
                     mesa.vn.debug=vtest mesa.vtest.socket.name=/dev/venus/venus.sock
                     debug.hwui.renderer=skiavk debug.renderengine.backend=skiaglthreaded
                     persist.waydroid.use_subsurface=true ro.surface_flinger.max_frame_buffer_acquired_buffers=3)
    else
        waydroid_settings+=("drm_device=$gpu")
        settings+=("gralloc.gbm.device=$gpu")
    fi
    # Refract's runtime Vulkan layer is a GPU debug layer, which Android loads only on a debuggable system (the
    # emulator's image is one). Waydroid turns it off by default.
    properties+=(ro.debuggable=1)
    # adb over the waydroid0 bridge, which install, check and play use. The image starts without it.
    properties+=(persist.sys.usb.config=adb service.adb.tcp.port=5555)

    # ARM64 translation. Digitalis' ARM64 bionic shares the host's bionic thread structures, so it runs only on the
    # Android 16 release whose bionic it was built with. prebuilts/digitalis is the Windows emulator's (QPR0 layout);
    # LineageOS 23.2 is QPR2, which needs prebuilts/digitalis-qpr2 (tools/translator/digitalis_build_qpr2.sh): with
    # the other one every ARM64 program crashes at start. The image's own ndk_translation is built for the image.
    # Digitalis' files lie in the overlay over the image's own ARM64 libraries, so they go when it is not chosen.
    local digitalis="$ROOT/prebuilts/digitalis-qpr2" bundle file
    if [[ $translator == digitalis ]]; then
        bundle="$digitalis/system"
        [[ -f "$bundle/lib64/libberberis_arm64.so" ]] ||
            die "no Digitalis bundle for Android 16 QPR2 at $digitalis: build it in WSL with tools/translator/digitalis_build_qpr2.sh (docs/digitalis_waydroid.md)"
        (cd "$digitalis" && sha256sum --quiet -c SHA256SUMS) || die "Digitalis bundle failed its checksums"
        while IFS= read -r -d '' file; do
            case "$file" in ./bin/*) install -D -m 755 "$bundle/$file" "$OVERLAY/system/$file" ;;
                            *) install -D -m 644 "$bundle/$file" "$OVERLAY/system/$file" ;; esac
        done < <(cd "$bundle" && find . -type f -print0)
        # The image's own translator (ndk_translation) would register the ARM64 executable handler first.
        install -d "$OVERLAY/system/etc/init"
        : >"$OVERLAY/system/etc/init/ndk_translation.rc"
        say "Installed Digitalis ($(basename "$digitalis")) into $OVERLAY/system"
    else
        # Either bundle's files (an earlier setup may have installed the QPR0 one).
        for bundle in "$ROOT/prebuilts/digitalis/system" "$digitalis/system"; do
            [[ -d "$bundle" ]] || continue
            while IFS= read -r -d '' file; do rm -f "$OVERLAY/system/$file"; done < <(cd "$bundle" && find . -type f -print0)
        done
        # Only the empty file setup wrote to mask the image's translator.
        [[ -f "$OVERLAY/system/etc/init/ndk_translation.rc" && ! -s "$OVERLAY/system/etc/init/ndk_translation.rc" ]] &&
            rm -f "$OVERLAY/system/etc/init/ndk_translation.rc"
    fi
    if [[ $translator == ndk ]]; then setup_ndk_shims install; else setup_ndk_shims remove; fi
    local bridge
    case "$translator" in
        digitalis) bridge=libberberis_arm64.so ;;
        ndk) bridge=libndk_translation.so ;;
        none) bridge=0 ;;  # The image's vendor default: no native bridge.
    esac
    properties+=("ro.dalvik.vm.native.bridge=$bridge")
    settings+=("ro.dalvik.vm.native.bridge=$bridge")
    if [[ $translator != none ]]; then
        properties+=(ro.dalvik.vm.isa.arm64=x86_64 ro.enable.native.bridge.exec=1
                     ro.product.cpu.abilist=x86_64,x86,arm64-v8a ro.product.cpu.abilist64=x86_64,arm64-v8a
                     ro.product.cpu.abilist32=x86)  # Android derives none of the three once abilist is set.
        [[ $translator == ndk ]] && say "Android will run ARM64 apps with the image's ndk_translation"
    else
        properties+=(ro.product.cpu.abilist=x86_64,x86 ro.product.cpu.abilist64=x86_64 ro.product.cpu.abilist32=x86)
    fi
    if [[ $identity == 1 ]]; then
        # Unity's OculusUnity checks for "oculus" in the manufacturer; Quest APKs then use Refract's
        # Meta Platform stand-in. As scripts/device_identity.ps1 does for the emulator.
        local partition
        for partition in "" system. vendor. odm. product. system_ext.; do
            properties+=("ro.product.${partition}brand=oculus" "ro.product.${partition}manufacturer=Oculus"
                         "ro.product.${partition}model=Quest 3")
        done
        say "Android will report a Meta Quest 3"
    fi

    python3 -I - "$CFG" "$venus" "${#waydroid_settings[@]}" "${waydroid_settings[@]}" "${properties[@]}" <<'PY'
import configparser, sys
path, venus, count = sys.argv[1], sys.argv[2] == "1", int(sys.argv[3])
settings, properties = sys.argv[4:4 + count], sys.argv[4 + count:]
cfg = configparser.ConfigParser(interpolation=None)
cfg.optionxform = str
cfg.read(path)
for section in ("waydroid", "properties"):
    if not cfg.has_section(section):
        cfg.add_section(section)
if venus:
    cfg.remove_option("waydroid", "drm_device")
else:  # Back from Venus to Mesa: Android's own drivers again.
    for key in ("mesa.vn.debug", "mesa.vtest.socket.name", "debug.hwui.renderer", "debug.renderengine.backend",
                "ro.hardware.gralloc", "ro.hardware.egl", "ro.hardware.vulkan"):
        cfg.remove_option("properties", key)
for section, pairs in (("waydroid", settings), ("properties", properties)):
    for pair in pairs:
        key, _, value = pair.partition("=")
        cfg.set(section, key, value)
with open(path, "w") as f:
    cfg.write(f)
PY
    write_waydroid_settings "$user" "$home" "${settings[@]}"
    trust_adb_key "$user" "$home"
    # Android loads Refract's runtime Vulkan layer from here (tools/android_runtime_policy.py puts it there).
    # Waydroid's adbd never runs as root, so the directory belongs to Android's shell user, which adb runs as.
    local layers="$home/.local/share/waydroid/data/local/debug"
    install -d -o 2000 -g 2000 -m 755 "$layers" "$layers/vulkan"
    # Regenerates waydroid_base.prop (which ends with waydroid.cfg's [properties]) and the container config.
    waydroid upgrade --offline >/dev/null
    if [[ $venus == 1 ]]; then
        # Waydroid has no setting for these, and 'waydroid upgrade' rewrites the file: check notices.
        local entry
        while IFS= read -r entry; do
            grep -qxF "$entry" "$LXC_NODES" || echo "$entry" >>"$LXC_NODES"
        done < <(venus_mounts "$gpu")
    fi

    # Android connects to the PC on the waydroid0 bridge (poses 38490, images 38491).
    if have firewall-cmd && firewall-cmd --state >/dev/null 2>&1; then
        local zone port
        zone="$(firewall-cmd --get-zone-of-interface=waydroid0 2>/dev/null || firewall-cmd --get-default-zone)"
        for port in "${BRIDGE_PORTS[@]}"; do
            if ! firewall-cmd --zone="$zone" --query-port="$port/tcp" >/dev/null 2>&1; then
                firewall-cmd --zone="$zone" --add-port="$port/tcp" >/dev/null
                firewall-cmd --permanent --zone="$zone" --add-port="$port/tcp" >/dev/null
                say "Opened TCP $port to Android in firewalld zone $zone"
            fi
        done
    fi
    # SELinux (Fedora): Android runs as container_runtime_t, but what it starts from its APEX packages (artd, the
    # media codecs, keystore HALs) runs as spc_t, and the host policy blocks binder calls between the two, so
    # Android's boot waits for artd forever. Allow exactly that.
    if have selinuxenabled && selinuxenabled && have semodule; then
        local module=/usr/local/lib/refract/refract_waydroid.cil
        install -d "$(dirname "$module")"
        printf '%s\n' '(allow spc_t container_runtime_t (binder (call transfer)))' \
                      '(allow container_runtime_t spc_t (binder (call transfer)))' >"$module"
        if semodule -i "$module"; then say "Allowed binder calls within Waydroid's container (SELinux module refract_waydroid)"
        else warn "could not install the SELinux module $module; Android may not finish booting"; fi
    fi
    systemctl enable waydroid-container >/dev/null 2>&1 || true
    systemctl restart waydroid-container
    say "Setup done. Next, as $user: waydroid session start, then scripts/waydroid.sh install and scripts/waydroid.sh check"
}

cmd_install() {
    session_running || die "start Waydroid first: waydroid session start"
    connect_adb
    local apk
    for apk in "${RUNTIME_APKS[@]}"; do
        [[ -f "$apk" ]] || die "$apk is missing: build it with ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh and platform-sdk/build_apk.sh"
    done
    for apk in "${RUNTIME_APKS[@]}" "$@"; do
        say "Installing $(basename "$apk")"
        adb_s install --no-incremental --force-queryable -r "$apk" | tail -n1
    done
}

# The activity a launcher would start: the normal launcher entry, else Meta's VR category.
resolve_activity() {
    local category found
    for category in android.intent.category.LAUNCHER com.oculus.intent.category.VR; do
        found="$(adb_s shell cmd package resolve-activity --brief -a android.intent.action.MAIN -c "$category" "$1" 2>/dev/null |
                 tr -d '\r' | grep '/' | tail -n1)"
        [[ -n "$found" ]] && { echo "$found"; return 0; }
    done
    return 1
}

# The app's name from its installed APK, as run_windows_game.ps1 reads it. Android's uid 1000 is this user, so the
# APK in Waydroid's data is readable here without copying it out.
app_label() {
    local apk aapt
    apk="$(adb_s shell pm path "$1" 2>/dev/null | tr -d '\r' | sed -n 's|^package:\(/data/.*/base\.apk\)$|\1|p' | head -n1)"
    apk="$USER_HOME/.local/share/waydroid/data${apk#/data}"
    aapt="$(command -v aapt2 || ls -d "${ANDROID_HOME:-$USER_HOME/Android/Sdk}"/build-tools/*/aapt2 2>/dev/null | sort -V | tail -n1)"
    [[ -f "$apk" && -x "$aapt" ]] || return 1
    "$aapt" dump badging "$apk" 2>/dev/null | sed -n "s/^application-label:'\(.*\)'\$/\1/p" | head -n1
}

# Starts a session process with its output in the run's logs; sets SESSION_PID.
start_logged() {  # log-name command...
    local name="$1"
    shift
    "$@" >"$SESSION_LOGS/$name.log" 2>"$SESSION_LOGS/$name.err" &
    SESSION_PID=$!
}
last_error() { grep -v '^[[:space:]]*$' "$SESSION_LOGS/$1.err" 2>/dev/null | tail -n1 || true; }

cmd_play() {
    local package="$DEFAULT_PACKAGE"
    [[ $# -gt 0 && "$1" != -* ]] && { package="$1"; shift; }
    local activity="" name="" owned=0 eye=1600 headset=0 scale=100 unreal_policy=1 run="waydroid-$(date +%Y%m%d-%H%M%S)"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --activity) activity="${2:?}"; shift 2 ;;
            --name) name="${2:?}"; shift 2 ;;
            --owned) owned=1; shift ;;
            --eye-size) eye="${2:?}"; shift 2 ;;
            --headset) headset=1; shift ;;
            --render-scale) scale="${2:?}"; shift 2 ;;
            --runtime) export XR_RUNTIME_JSON="${2:?}"; shift 2 ;;
            --run) run="${2:?}"; shift 2 ;;
            --no-unreal-policy) unreal_policy=0; shift ;;
            *) die "unknown play option: $1" ;;
        esac
    done
    [[ "$package" =~ ^[A-Za-z0-9_.]+$ ]] || die "not a package name: $package"
    [[ "$eye" =~ ^[0-9]+$ && "$eye" -ge 512 && "$eye" -le 4096 ]] || die "--eye-size must be 512-4096"
    [[ "$scale" =~ ^[0-9]+$ && "$scale" -ge 25 && "$scale" -le 200 ]] || die "--render-scale must be 25-200"
    if [[ $headset == 1 ]]; then [[ -x "$HOST_BRIDGE" ]] || die "build the host bridge first: ./build_host.sh"
    else [[ -x "$VIEWER" ]] || die "build the PC viewer first: ./build_host.sh"; fi
    SESSION_LOGS="$ROOT/runs/$run"
    mkdir -p "$SESSION_LOGS"

    local gpu_share=0
    if venus_mode; then
        # Android's graphics need the renderer before Waydroid starts.
        systemctl --user is-active --quiet "$VENUS_UNIT" || systemctl --user start "$VENUS_UNIT" ||
            die "the Venus renderer did not start: journalctl --user -u $VENUS_UNIT"
        # The PC viewer imports the eye images Refract's GPU layer shares from the renderer (as on Windows).
        [[ $headset == 0 && -f "$GPU_LAYER_DIR/librefract_gpu_layer.so" ]] && gpu_share=1
    fi
    if ! session_running; then
        say "Starting Waydroid"
        nohup waydroid session start >"$SESSION_LOGS/waydroid-session.log" 2>&1 &
        for _ in $(seq 90); do session_running && break; sleep 1; done
        session_running || die "Waydroid's session did not start (log: runs/$run/waydroid-session.log)"
    fi
    connect_adb
    local host
    host="$(host_address)"
    [[ -n "$host" ]] || die "no waydroid0 network bridge; is Waydroid's container running?"
    adb_s shell pm path "$RUNTIME_PACKAGE" >/dev/null 2>&1 || die "Refract's runtime is not installed: scripts/waydroid.sh install"
    adb_s shell pm path "$package" >/dev/null 2>&1 || die "$package is not installed: scripts/waydroid.sh install game.apk"
    if [[ -z "$activity" ]]; then
        activity="$(resolve_activity "$package")" || die "found no activity to start in $package; pass --activity"
    elif [[ "$activity" != */* ]]; then
        activity="$package/$activity"
    fi
    [[ -n "$name" ]] || name="$(app_label "$package" || true)"
    [[ -n "$name" ]] || name="$package"

    # The game's ownership, as tools/run_windows_game.ps1 decides it.
    local owned_file="$ROOT/scripts/owned_games.txt" user_file="$ROOT/scripts/platform_user_id.txt"
    if [[ $owned == 0 && -f "$owned_file" ]] && grep -v '^[[:space:]]*#' "$owned_file" | tr -d ' \t\r' | grep -qxF "$package"; then
        owned=1
    fi
    local user_id=""
    [[ -f "$user_file" ]] && user_id="$(grep -v '^[[:space:]]*#' "$user_file" | tr -d ' \t\r' | grep -m1 . || true)"

    # Session properties, as run_windows_game.ps1 sets them. debug.refract.host_addr points the runtime straight
    # at this PC (the emulator uses adb reverse instead).
    local -a props=(
        "debug.refract.host_addr=$host"
        "debug.refract.gpu_share=$gpu_share"
        "debug.refract.video=''"  # Pixels or shared images: nothing on Linux decodes H.264.
        "debug.refract.platform.owned.$package=$owned"
        "debug.refract.runtime_name=Oculus"
        "debug.refract.texture_mip_limit=$([[ "$package" == com.camouflaj.manta ]] && echo 1 || echo 0)"
    )
    if [[ $headset == 1 ]]; then
        props+=("debug.refract.composite=1" "debug.refract.frame_sync=1" "debug.refract.hfov=''")
    else
        props+=("debug.refract.composite=1" "debug.refract.frame_sync=0" "debug.refract.hfov=109"
                "debug.refract.stream_eyes=2" "debug.refract.stream_scale=100" "debug.refract.direct_host=0")
    fi
    [[ -n "$user_id" ]] && props+=("debug.refract.platform.user_id=$user_id")
    local entry
    for entry in "${props[@]}"; do
        adb_s shell setprop "${entry%%=*}" "${entry#*=}" || die "could not set ${entry%%=*}"
    done
    # As Refract's launcher sets them on Windows (launcher/core/runtime.mjs). A new Android's one-time "Viewing full
    # screen" notice takes focus, and a Unity game then pauses itself and never renders. Nobody can tap a system
    # app's crash or "not responding" dialog either, and it keeps the game from becoming the resumed activity.
    adb_s shell 'settings put secure immersive_mode_confirmations confirmed; settings put global hide_error_dialogs 1' >/dev/null 2>&1 ||
        warn "could not hide Android's full-screen notice and error dialogs"
    # Unreal games get a texture pool sized to the GPU's memory (docs/windows_unreal_memory.md). Only Unreal games
    # are changed, and only they need root (sudo) for it.
    local -a memory_args=(--adb "$ADB" --serial "$SERIAL" --package "$package" --state-dir "$ROOT/build-waydroid-game/memory-policy")
    [[ $unreal_policy == 1 ]] || memory_args+=(--restore)
    local memory
    memory="$(python3 "$ROOT/tools/unreal_memory_policy.py" "${memory_args[@]}")" ||
        die "the Unreal memory policy failed; the game was not started"
    say "Unreal memory policy: $memory"
    local policy
    policy="$(python3 "$ROOT/tools/android_runtime_policy.py" --adb "$ADB" --serial "$SERIAL" --package "$package")" ||
        die "the Android runtime policy failed; the game was not started"
    say "Android runtime policy: $policy"

    # A leftover pose server, viewer or bridge holds the pose and image ports and would take the game's connection.
    pkill -f "refract-host-bridge --serve-openxr" 2>/dev/null || true
    pkill -f "scripts/pose_input_server.py" 2>/dev/null || true
    pkill -x refract-viewer 2>/dev/null || true
    SESSION_PACKAGE="$package"
    trap 'stop_session; exit 130' INT TERM
    trap stop_session EXIT
    adb_s logcat -c || true
    adb_s logcat -v threadtime >"$SESSION_LOGS/logcat.txt" 2>&1 &
    SESSION_LOGCAT_PID=$!

    if [[ $headset == 1 ]]; then
        say "Starting the host bridge (log: runs/$run/host.log)"
        REFRACT_RENDER_SCALE="$scale" start_logged host "$HOST_BRIDGE" --serve-openxr "${BRIDGE_PORTS[0]}" 0 "$name"
        SESSION_PIDS+=("$SESSION_PID")
        SESSION_MAIN_PID=$SESSION_PID
        # Start the game only once the bridge has an OpenXR session (it then prints the tracking origin).
        for _ in $(seq 60); do
            grep -q 'tracking origin=' "$SESSION_LOGS/host.log" "$SESSION_LOGS/host.err" 2>/dev/null && break
            kill -0 "$SESSION_MAIN_PID" 2>/dev/null || break
            sleep 0.25
        done
        if ! kill -0 "$SESSION_MAIN_PID" 2>/dev/null; then
            local failure
            failure="$(last_error host)"
            case "$failure" in
                *XR_ERROR_FORM_FACTOR_UNAVAILABLE*) die "no VR headset is connected to the PC OpenXR runtime" ;;
                *XR_ERROR_RUNTIME_UNAVAILABLE*|*XR_ERROR_RUNTIME_FAILURE*|*xrCreateInstance*|*libopenxr_loader*)
                    die "the PC OpenXR runtime is not running or not found (start SteamVR or Monado; log: runs/$run/host.err)" ;;
                *) die "the host bridge stopped: $failure (log: runs/$run/host.err)" ;;
            esac
        fi
    else
        # Head and controller poses from the keyboard and mouse (through the viewer). The game reads its eye
        # size from the first pose connection, so the server must listen before the game starts.
        start_logged input python3 "$ROOT/scripts/pose_input_server.py" --eye-width "$eye" --eye-height "$eye" --refresh-rate 250
        SESSION_PIDS+=("$SESSION_PID")
        for _ in $(seq 40); do
            ss -Hltn "sport = :${BRIDGE_PORTS[0]}" 2>/dev/null | grep -q . && break
            kill -0 "$SESSION_PID" 2>/dev/null || break
            sleep 0.25
        done
        kill -0 "$SESSION_PID" 2>/dev/null || die "the PC input server could not start: $(last_error input) (log: runs/$run/input.err)"
        local shots="$(xdg-user-dir PICTURES 2>/dev/null || echo "$HOME/Pictures")/Refract"
        # --adb/--serial/--package: Android CPU and the game's busiest threads in the F1 overlay, as on Windows.
        start_logged viewer "$VIEWER" --title "$name" --shots "$shots" --adb "$ADB" --serial "$SERIAL" --package "$package"
        SESSION_PIDS+=("$SESSION_PID")
        SESSION_MAIN_PID=$SESSION_PID
        sleep 0.8
        kill -0 "$SESSION_MAIN_PID" 2>/dev/null || die "the PC viewer could not start: $(last_error viewer) (log: runs/$run/viewer.err)"
    fi

    # Nobody can answer a crash or permission dialog in a headset: close stray dialogs and grant permissions first.
    adb_s shell 'for i in 1 2 3 4 5; do dumpsys window | grep mCurrentFocus | grep -qE "Application.Error|Not.Responding|isn.t.responding|keeps.stopping" || break; am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS >/dev/null 2>&1; input keyevent KEYCODE_BACK; sleep 1; done' || true
    adb_s shell pm grant --all-permissions "$package" >/dev/null 2>&1 || true
    local launch
    launch="$(adb_s shell am start -W -n "$activity" 2>&1 | tr -d '\r')"
    [[ "$launch" != *Error:* ]] || die "the game did not start: $launch"
    SESSION_GAME_STARTED=1
    if [[ $headset == 1 ]]; then say "$name is running ($activity). Ctrl+C stops the session."
    else say "$name is running ($activity), GPU sharing $([[ $gpu_share == 1 ]] && echo on || echo off). Close the window or press Ctrl+C to stop."; fi

    local missing=0 game_pid="" running
    while kill -0 "$SESSION_MAIN_PID" 2>/dev/null; do
        running="$(adb_s shell pidof "$package" 2>/dev/null | tr -d '\r' || true)"
        if [[ -n "$running" ]]; then missing=0; game_pid="${running%% *}"; else missing=$((missing + 1)); fi
        if [[ $missing -ge 2 ]]; then
            # Only the game's own log lines: another app or a system service crashing meanwhile is not the game's
            # crash. The log has binary bytes (grep -a). Under ndk_translation a native crash shows as the game's
            # "signal 11 (SIGSEGV)" lines (Unity's CRASH, debuggerd's DEBUG), with no "Fatal signal".
            local game_log="" crash="" abort=""
            [[ -n "$game_pid" ]] && game_log="$(awk -v pid="$game_pid" '$3 == pid' "$SESSION_LOGS/logcat.txt" | tr -d '\000')"
            crash="$(grep -a -m1 -oE '(Fatal signal [0-9]+|FATAL EXCEPTION|HandleFatalSignal: sig=[0-9]+|signal [0-9]+ \(SIG[A-Z]+\)).*' <<<"$game_log" || true)"
            abort="$(grep -a -m1 -oE 'Abort message: .*' <<<"$game_log" || true)"
            [[ -n "$crash" && -n "$abort" ]] && crash="$crash. $abort"
            if [[ -n "$crash" ]] && grep -aqE 'VrApiLoader|VrApi Loader' <<<"$game_log"; then
                warn "$name is built on Meta's older VrApi SDK, which Refract does not support (only OpenXR games run)."
            elif [[ -n "$crash" ]]; then
                warn "$name crashed: $crash"
            else
                say "$name closed."
            fi
            SESSION_GAME_STARTED=0
            break
        fi
        sleep 1
    done
    stop_session
}

# A play session's processes, global so the exit trap still sees them after cmd_play returns.
SESSION_PACKAGE="" SESSION_LOGS="" SESSION_PID="" SESSION_MAIN_PID="" SESSION_LOGCAT_PID="" SESSION_GAME_STARTED=0 SESSION_STOPPED=0
SESSION_PIDS=()
stop_session() {
    [[ $SESSION_STOPPED == 0 ]] || return 0
    SESSION_STOPPED=1
    if [[ $SESSION_GAME_STARTED == 1 ]]; then
        # Background the game first so it gets onPause/onStop (and saves) while the viewer or bridge still runs.
        adb_s shell am start -W -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1 || true
        sleep 2
        adb_s shell sync >/dev/null 2>&1 || true
        adb_s shell am force-stop "$SESSION_PACKAGE" >/dev/null 2>&1 || true
    fi
    local pid
    for pid in "${SESSION_PIDS[@]}"; do
        # SIGTERM ends the bridge's frame loop, so it closes its OpenXR session properly.
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${SESSION_PIDS[@]}"; do
        for _ in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$pid" 2>/dev/null || true
    done
    [[ -n "$SESSION_LOGCAT_PID" ]] && kill "$SESSION_LOGCAT_PID" 2>/dev/null || true
    say "Refract game session stopped."
}

case "${1:-}" in
    check) shift; cmd_check "$@" ;;
    setup) shift; cmd_setup "$@" ;;
    install) shift; cmd_install "$@" ;;
    play) shift; cmd_play "$@" ;;
    *) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2 ;;
esac
