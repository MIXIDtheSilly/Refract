<p align="center">
  <img src="img/refract_banner.png" alt="Refract" width="720">
</p>

Refract runs Android (Meta Quest) OpenXR apps on a Windows PC and shows them in a PC VR
headset. The app runs in the Android Emulator. Refract provides an Android OpenXR runtime inside the
emulator, which sends frames to the PC and receives headset and controller poses back.

> **Status:** experimental. It works for the setups documented below, but compatibility with arbitrary games is unfinished.

## How it works

```text
Android OpenXR APK (in the Android Emulator)
  -> Refract Android OpenXR runtime (com.refract.openxrruntime)
  -> Refract protocol (poses, frames, input)
  -> Refract viewer / host bridge (Windows)
  -> PC OpenXR runtime (SteamVR)
  -> headset
```

The eye images are shared through a Vulkan layer that is loaded into the emulator. A
translator runs ARM64-only games on the x86_64 emulator image.

## Repository layout

```text
android-runtime/      Android OpenXR runtime (native)
android-runtime-apk/  Installable runtime APK and runtime broker
platform-sdk/         Meta Platform SDK stand-in (package com.oculus.horizon)
protocol/             Pose, image and input protocol and transports
viewer/               Windows viewer: receives the shared eye textures and shows them
host-bridge/          Host bridge to a PC OpenXR runtime
launcher/             Desktop launcher (Tauri + Node backend, GPL-3.0)
scripts/              Start the emulator and launch a game
tools/                GPU layer, GLES layer, translator patches, probes, analysis
prebuilts/digitalis/  Prebuilt ARM64-to-x86_64 translator (Digitalis/Berberis)
tests/                Smoke tests and loader probes
docs/                 Setup guides and design notes
```

## Getting started

Requirements: Windows 11, an Nvidia GPU, the Android SDK with the Android Emulator and NDK,
CMake and Ninja, and SteamVR.

1. Build and set up the emulator path. See
   [docs/windows_nvidia_emulator.md](docs/windows_nvidia_emulator.md).
2. Build the viewer with `viewer\build.bat`.
3. Start a game. [docs/yeeps_setup.md](docs/yeeps_setup.md) walks through a full working
   configuration:

   ```powershell
   .\scripts\start_emulator.ps1 -Cores 6 -Hidden
   .\scripts\ensure_tsc.ps1
   .\scripts\launch.ps1 -Run my-run -TranslatorMode two-gear
   ```

   Logs for each run are written to `runs\<name>\`.

Architecture, status and build details are in [docs/overview.md](docs/overview.md).

> **Note:** several scripts still hardcode the author's Android SDK path
> (`C:\Users\mixid\Android\Sdk`). Change it to match your machine.

## Not included

This repository contains no game APKs, Meta SDKs or Horizon OS files. You need to provide your own:

- The APKs of games you own.
- The OpenXR loader AAR, under `third_party/`.

The ARM64 translator is included prebuilt in [prebuilts/digitalis/](prebuilts/digitalis/)
(Digitalis/Berberis, Apache-2.0, with Refract's performance patch), so you don't have to build
AOSP. `scripts\translator.ps1 -Use digitalis` installs it into the emulator.

To pass the entitlement check with the Meta Platform SDK stand-in, list the games you own in
`scripts/owned_games.txt`. [scripts/owned_games.example.txt](scripts/owned_games.example.txt) shows the format.

Refract is meant for testing apps you own and open samples. Do not use it to bypass DRM,
anti-cheat, platform security, store restrictions or application license terms.

## Credits and license

Refract started as a fork of [AXRB](https://github.com/Android-XR-Bridge/AXRB) by Feline Reintgen.
It has since grown well beyond it: Refract adds its own Windows launcher, the Vulkan and GLES
layers, the translator performance work, the Platform SDK stand-in and much more. See
[CREDITS.md](CREDITS.md).

Refract is licensed under the **GNU General Public License v3.0 or later** ([LICENSE](LICENSE)),
with one additional term for visible credit ([NOTICE](NOTICE)):

- **Projects built on Refract must stay open source.** If you distribute Refract, a modified
  version or a project that includes Refract code, you must release its full source code under
  GPL-3.0-or-later as well.
- **Using Refract code requires visible credit.** You must credit Refract and link to
  <https://github.com/MIXIDtheSilly/Refract> where users can see it (a README, an About or
  credits page, or release notes). See [NOTICE](NOTICE) for the exact wording.

Code that still comes from AXRB was released under the MIT License; its copyright notice is kept
in [NOTICE](NOTICE).
