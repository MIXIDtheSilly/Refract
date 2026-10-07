# Linux with Waydroid

On Linux, Refract runs Android in [Waydroid](https://waydro.id) instead of the Android Emulator.
Waydroid is a container: Android runs on the PC's own kernel, so there is no virtual machine. It runs
the same Android version as the Windows emulator, Android 16 (API 36), from
[WayDroid-ATV](https://github.com/WayDroid-ATV/waydroid-builds)'s LineageOS 23.2 images.

```text
Quest APK (ARM64, translated by the image's ndk_translation)
  -> Refract Android OpenXR runtime (in Waydroid)
  -> Android's Vulkan: Mesa Venus -> Unix socket -> virgl renderer on the host -> Nvidia's driver
       (Refract's GPU layer in the renderer shares the eye images, as on Windows)
  -> refract-viewer (a window on this PC), poses from scripts/pose_input_server.py
```

`play --headset` sends the images over TCP to `refract-host-bridge` and a PC OpenXR runtime
(SteamVR, Monado, WiVRn) instead.

> **Status:** tested on Fedora 44 with an RTX 3080. Android 16 boots, and it draws on the RTX 3080
> through Venus: SurfaceFlinger reports ANGLE on Vulkan 1.3 on the RTX 3080. The Quest 3 identity is
> active. Yeeps and Batman: Arkham Shadow run in the PC viewer with GPU sharing.

## The ARM64 translator

The Windows emulator translates ARM64 games with Digitalis. Waydroid uses the image's own
`ndk_translation` by default. `setup --translator digitalis` installs Digitalis instead, from a bundle
built for this image's Android version.

Digitalis' ARM64 bionic shares two structures with the host's (x86_64) bionic: `pthread_internal_t`
and `bionic_tls`. It works only on the Android 16 release whose bionic layout it was built with:

- **QPR0 (`BE2A`), the Windows emulator.** `prebuilts/digitalis` targets it. Digitalis' AOSP tree is
  QPR2, so Refract's WSL tree carries four local bionic commits that restore the QPR0 layout:
  - the revert of "Disable tracing during libc init.", which flips the systrace flag;
  - the revert of "Optimize android_unsafe_frame_pointer_chase", which removes `stack_bottom`;
  - the revert of the libgen TLS change, which puts 8 KB of buffers back into `bionic_tls`;
  - a build fix.
- **QPR2, LineageOS 23.2.** It needs `prebuilts/digitalis-qpr2`, built from the same tree without those
  commits by `tools/translator/digitalis_build_qpr2.sh` (in WSL, as root). The script builds and packs
  the bundle, then puts the commits back and rebuilds, so the Windows workflow is unchanged.
  [digitalis_waydroid.md](digitalis_waydroid.md) has the steps.

With the wrong bundle, the ARM64 linker reads `bionic_tls` 8 bytes off and gets a null pointer. Every ARM64
program, even `linker64 --list`, then crashes at start (fault address `0x2fa1`). `pthread_exit` shows the
shift: the emulator's x86_64 libc reads `0x98/0xa8/0xb0`, and LineageOS 23.2's reads `0xa0/0xb0/0xb8`.
The bundle's `linker64` shows which layout it has: it loads `bionic_tls` from offset `0x2f8` (QPR0) or
`0x300` (QPR2) of the thread structure.

`setup` installs the two fixes below only for `ndk_translation`. With Digitalis, games get the same
fixes as on Windows.

`ndk_translation` has no bridge for `JNI_GetCreatedJavaVMs` in the guest `libandroid_runtime.so`.
A call there aborts the app ("Bad 'JNI_GetCreatedJavaVMs' call"), and Meta's audio SDK
(`libMetaXRAudioUnity.so`) makes it by name from its metrics code. On Windows,
`tools/patch_northstar_windows.py` removes that call from North Star. For `ndk`, `setup` instead puts a
copy of the image's `libandroid_runtime.so` in the overlay that no longer exports the function. A
lookup then finds `libnativehelper.so`'s working bridge, or nothing.

The SDK's `android_namespace_loader` (in `libMetaXRAudioWwise.so`, for example Batman: Arkham Shadow)
calls libc's unexported `android_get_exported_namespace`. It finds the function by reading the
distance from `android_get_device_api_level` in the libc file on disk. On Waydroid that file is the
x86_64 host libc, so the call lands in unrelated ARM64 code. Windows fixes this inside Digitalis
(`tools/translator/digitalis-syslib-redirect.patch`). For `ndk`, `setup` patches the guest libc copy
instead. `android_get_device_api_level` is exported from a padding slot that branches to the real
function. The slot at the host's distance from it branches to the ARM64
`android_get_exported_namespace`.

Only the known libraries (by SHA-256) are changed.

## Requirements

- A kernel with Android binder (`binderfs`). Fedora, Arch, Ubuntu and Debian kernels have it.
- Waydroid 1.6.3 or newer. On Fedora, `setup` installs it.
- A GPU:
  - **Nvidia, with Nvidia's own driver:** Android uses it through Venus
    ([waydroid-nvidia](https://github.com/Shiro836/waydroid-nvidia)). `setup` downloads and
    installs it.
  - **AMD, Intel, or Nvidia on `nouveau`:** Android uses Waydroid's Mesa drivers.
- adb (Fedora: `android-tools`, or the Android SDK's `platform-tools`).
- To build: CMake, Ninja, a C++20 compiler, Vulkan and OpenXR headers, and for the Android packages
  the Android SDK and NDK 27.3 plus a JDK.

`scripts/waydroid.sh check` reports which of these are missing. It changes nothing.

## Build

```sh
./build_host.sh                                         # build-linux/: refract-viewer, gpu-layer, host bridge
ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh  # runtime + com.oculus.systemdriver
platform-sdk/build_apk.sh                               # Meta Platform SDK stand-in
```

The APK scripts read the SDK from `ANDROID_HOME` (default `~/Android/Sdk`).

## Set up Waydroid (once)

```sh
sudo scripts/waydroid.sh setup
waydroid session start
scripts/waydroid.sh install                             # Refract's three packages
scripts/waydroid.sh install ~/Games/MyGame.apk          # your own games
scripts/waydroid.sh check
```

`setup` makes these changes:

- **Android 16:** downloads WayDroid-ATV's images (checked by SHA-256) into
  `/etc/waydroid-extra/images`, then runs `waydroid init` with them. Waydroid's updater leaves images
  there alone. `--images DIR` uses your own `system.img` and `vendor.img`.
- **Venus (Nvidia):**
  - Installs waydroid-nvidia's renderer in `/usr/local/lib/refract`, and its Android Venus driver,
    gralloc and ANGLE in Waydroid's overlay. Its LineageOS 20 hwcomposer and surfaceflinger are left
    out.
  - Adds the user service `refract-venus.service`, which runs the renderer on the chosen GPU with
    Refract's GPU layer loaded.
  - Adds two bind mounts to `/var/lib/waydroid/lxc/waydroid/config_nodes`: the render node, and the
    renderer's socket directory `/run/refract-venus` (seen as `/dev/venus`). `waydroid upgrade`
    rewrites that file, so run `setup` again after one.
- **ARM64 translator:** uses the image's `ndk_translation`, and removes Digitalis from Waydroid's
  overlay if an earlier setup put it there. See [The ARM64 translator](#the-arm64-translator).
- **Properties** in `/var/lib/waydroid/waydroid.cfg`:
  - the native bridge and the ABI lists (with `arm64-v8a`);
  - the Quest 3 identity that unmodified Quest APKs check for, as `scripts/device_identity.ps1`
    sets on Windows;
  - `ro.debuggable=1`, so Android loads the runtime's Vulkan layer;
  - adb over TCP 5555.

  Graphics settings also go to `~/.local/share/waydroid/data/misc/waydroid_settings`, which the
  ATV vendor image reads.
- **adb:** adds your adb key (`~/.android/adbkey.pub`) to Android's trusted keys. Nothing on the PC
  would show Android's "Allow USB debugging?" prompt.
- **Firewall:** opens TCP 38490 and 38491 to `waydroid0` in firewalld.
- **SELinux (Fedora):** the module `refract_waydroid` lets Android's APEX services (`spc_t`) and
  the rest of Android (`container_runtime_t`) make binder calls to each other. Without it, Android's
  boot waits for `artd` forever.
- Restarts Waydroid's container.

The options:
- `--gpu /dev/dri/renderDN` chooses the GPU. The default is the one driving the desktop.
- `--translator ndk|digitalis|none` chooses the ARM64 translator. The default is `ndk`. `digitalis`
  needs `prebuilts/digitalis-qpr2` (see [The ARM64 translator](#the-arm64-translator)).
  `--no-translator` is the same as `none`.
- `--no-identity` leaves out the Quest 3 identity.

Waydroid freezes Android's container when Android's screen goes off, which happens when no Waydroid
window is open. A frozen Android accepts adb connections but never answers them. The script's adb
connection thaws the container (through Waydroid's D-Bus service, which needs no root) and keeps
the screen on.

## Play

```sh
scripts/waydroid.sh play                                # North Star
scripts/waydroid.sh play com.example.game --name "Example Game"
```

`play` performs these steps:

1. Starts the Venus renderer and Waydroid's session if they are not running, and connects adb.
2. Sets the session's `debug.refract.*` properties as `tools/run_windows_game.ps1 -PcViewer` does.
   `debug.refract.host_addr` points the runtime at this PC's `waydroid0` address. It also marks Android's
   one-time "Viewing full screen" notice as seen and hides system crash dialogs, as the Windows launcher
   does. Nobody could dismiss them, and they keep the game from becoming the active app.
3. Applies the Unreal texture-pool policy and the runtime policy (`tools/android_runtime_policy.py --adb`).
4. Starts the pose server and `refract-viewer`, then the game.

The window title is the app's name from its APK unless `--name` is given. When the game exits, `play`
reports a crash only from the game's own log lines.

### Texture memory

The texture fixes from Windows apply here too:

- **Compressed textures:** Refract's Vulkan layer turns ETC2/EAC/ASTC textures into BC textures that
  the RTX 3080 samples natively (`debug.refract.transcode_textures`).
- **Batman: Arkham Shadow:** the runtime drops the top mip level of Unity's textures
  (`debug.refract.texture_mip_limit`). It finds Unity's `il2cpp_resolve_icall` in `libil2cpp.so`'s own
  symbol table, because ndk_translation's linker won't `dlopen` a library from another linker namespace.
- **Unreal games:** `tools/unreal_memory_policy.py` sizes the texture pool from the GPU's memory, as on
  Windows (see [windows_unreal_memory.md](windows_unreal_memory.md)). Venus shows Android every GPU in the
  PC, with its real memory. The policy uses the first GPU, the one games use. Its VRAM is the heap of its
  device-local, non-host-visible memory. Venus also marks system memory as device-local, so the pool
  percentage is computed against all device-local heaps, which Unreal adds up.
  - Writing the game's `Engine.ini` needs root. The policy uses `sudo waydroid shell`, so `play` asks for
    your password, but only for Unreal games.
  - The backup is in `build-waydroid-game/memory-policy/`, and `play --no-unreal-policy` restores the
    original.
  - No Unreal game has been tested on Linux yet.

Closing the window or pressing Ctrl+C stops the game. F1 shows the performance overlay, as on
Windows: game and viewer frame rates, frame times with a graph, render size, CPU use and the game's
busiest threads. Waydroid shares the PC's kernel, so its CPU line is the whole PC's. Logs are in `runs/<run>/`.

`--no-unreal-policy` restores an Unreal game's own texture pool. Ownership and the Meta user ID come from `scripts/owned_games.txt` and
`scripts/platform_user_id.txt`, as on Windows, or from `--owned`. `--eye-size` sets the window's
render size per eye.

`--headset` uses the host bridge and a PC OpenXR runtime instead. With it, `--render-scale` and
`--runtime file.json` (which sets `XR_RUNTIME_JSON`) are also available. SteamVR from Flathub keeps
its runtime inside the Flatpak sandbox, so a host bridge outside it may not reach it. A native Steam
install, Monado or WiVRn avoids that.

Not on Linux yet: the loading card, the in-headset stats panel, the desktop launcher, and Steam Link.
