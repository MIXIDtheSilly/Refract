# Refract launcher

A native Windows desktop frontend for the Refract runtime: your installed Android
games, owned Quest game downloads, expansion files and DLC.

## Run

Double-click **Refract Launcher.cmd** (or run
`powershell -ExecutionPolicy Bypass -File tools/run_launcher.ps1` from the project root).
On Windows 11 that is all: missing Node.js, Python, Rust and the Visual Studio C++
build tools are installed with winget, the locked npm dependencies are installed,
and `src-tauri/target/release/refract-launcher.exe` is built (a few minutes the first
time; later runs rebuild only when the UI or shell sources changed). A launcher
that is already open with older sources is closed and started again.

Everything else is under **Settings > Setup**, which checks this PC and fixes what
it can with one click: Python, Android, the `refract-google-api36` AVD, Windows
Hypervisor Platform, Refract's own build outputs (host bridge, PC viewer, GPU layer,
Android runtime and Meta Platform stand-in APKs) and the PC's OpenXR runtime.
The Library shows a banner while anything is missing.

**Set up Android** (`core/android_sdk.mjs`) needs no Android Studio or Java. It downloads
the exact packages Refract is tested with from Google's SDK repository, checking each
one's SHA-1: emulator 37.1.11 (build 15917651), the Google APIs x86_64 Android 16 image
revision 7 (the build the Digitalis translator matches), build-tools 36 and platform-tools.
It then writes the multi-core qemu copy (see `tools/patch_emulator_cores.py`) and creates
the AVD. A new AVD's first boot runs once on the stock single-core emulator, because the
multi-core one is killed by the emulator's hang detector while Android encrypts `/data`.
The first install or play then installs Digitalis and sets the Quest device identity
(brand `oculus`, manufacturer `Oculus`, model `Quest 3`); each restarts Android once.

## Share a build

`powershell -ExecutionPolicy Bypass -File tools/package_release.ps1` builds the launcher
and writes `dist/Refract-<version>-win64.zip` (about 50 MB). Unzipped on another PC,
`Refract.exe` runs without a source checkout or developer tools. It carries its own
Node.js, the built host programs, the OpenXR loader and Visual C++ runtime for the host
bridge, the Android-side APKs and Digitalis. Android itself (about 2.4 GB) is downloaded
by **Set up Android** on that PC. `tools/package_readme.txt` becomes the package's
README.txt for the person receiving it.

## How it is put together

- `src/` is the interface (React, plain CSS, the Jost typeface). Gray surfaces,
  white controls and the logo from `img/Refract_logo_trans.svg`.
- `src-tauri/` is the native shell (Rust, Tauri 2). It owns the window, file
  dialogs, the Meta sign-in window, opening folders/links and the encrypted Meta
  session. The UI has one command, `call`, and only the main window may use it.
- `backend/server.mjs` holds the launcher logic (library, Meta account, downloads,
  install, play) on top of `core/`. The shell starts it with Node and
  talks to it with one JSON message per line over stdin/stdout. It is read from
  the repository, so backend changes need no rebuild.

### Develop

```powershell
cd launcher
npm ci
npm start            # tauri dev: Vite with hot reload inside the real window
npm run dev          # the UI alone in a browser, running on sample data
npm run preview:build  # dist-preview/index.html: one self-contained preview file
```

Outside Tauri the UI uses `src/api/mock.js`, so it can be designed and reviewed
in any browser without Windows, Android or a Meta account.

## Use

- **Library:** Refresh imports launchable apps from the already-running selected
  AVD. It does not start the emulator just to scan. Cached games stay visible when
  Android is stopped. Install/play starts Android as needed.
- **Connect Meta:** Sign in to list your Quest entitlements in the library and
  download them; a Rift purchase is not a Quest entitlement. Sign in on Meta's hosted page, in a separate private window
  with no access to the launcher. Credentials are never sent to a Refract service.
  The account token is encrypted with Windows DPAPI and never sent to the
  launcher UI or written in logs.
- **Downloads:** Choose a Quest build. APK, OBB and binary asset files are saved
  with original filenames. Transfers can be cancelled/retried, incomplete files
  stay `.part`, resume requires an ETag, and completed files receive a local
  SHA-256 for verification before installation. The download folder is selectable.
- **Game page:** Click a game to open its page. Versions, add-ons,
  content imports and installation updates are under **Manage**.
- **Add-ons:** Ownership must be returned by Meta before a separate DLC download
  is enabled. Some DLC is only an entitlement to content inside the base game,
  with no downloadable file.
- **Local games:** Import an APK and optional expansion files. Originals are
  referenced in place, not deleted or modified.
- **Install:** Uses `adb install -r`, preserving app data. Signature conflicts
  report an error; the launcher does not uninstall the existing app. Assets are
  pushed into `/sdcard/Android/obb/<package>/`. After downloading more content,
  use **Update installation** to copy it into Android.
- **Play in VR:** Calls `tools/run_windows_game.ps1`, preserving automatic OpenXR eye
  resolution, SteamVR name/icon, GPU texture sharing, and the save-aware shutdown.
  Play first asks the PC VR runtime for a headset (`refract-host-bridge --probe-openxr`),
  so a disconnected headset or a stopped Meta Horizon Link/SteamVR is reported in about
  a second, before Android starts. The session starts the game only after the bridge
  has an OpenXR session.
- **Play on PC:** The same script with `-PcViewer`: the game shows in a window on this PC
  (`viewer/build/refract_viewer.exe`, built by `viewer/build.bat`) and takes input from the
  keyboard, mouse and Xbox controllers (`scripts/pose_input_server.py`). The game page
  lists the controls. Screenshots (F2) go to `Pictures\Refract`.
- **Headset status:** The library checks for a headset every 15 s while no game runs
  (SteamVR only while it is open, since asking it would start it). With none connected
  it says so, explains how to connect, and puts **Play on PC** first.
- Closing the game window (VR preview or PC viewer) stops the game. Closing the launcher
  does not intentionally stop a running game.

The current default AVD is `refract-google-api36` on port 5580 with 8 GB guest RAM. It must be an
Android 16 (API 36) AVD: games run on the Digitalis ARM64 translator, which is built for Android 16.
The Android SDK is found automatically (`ANDROID_HOME`, `ANDROID_SDK_ROOT`,
`%LOCALAPPDATA%\Android\Sdk`, `~\Android\Sdk`). Change these in Settings for another setup.

Before installing or playing, the launcher gets Android ready: it uses an emulator
that already runs the selected AVD on any port (for example one from
`scripts/start_emulator.ps1`, even while it is still booting) instead of starting a
second copy, which the emulator refuses. Otherwise it starts one. It then installs
Digitalis if missing (`scripts/translator.ps1`, reboots Android) and installs or
updates the Refract OpenXR runtime and Meta Platform stand-in APKs whenever the
built APK differs from the installed one. Games Meta lists as yours are reported
to them as owned; `scripts/owned_games.txt` is only needed for other games.
Script failures are shown as one plain message (for example why the emulator
quit), not PowerShell's error record.

## Data and limitations

`%APPDATA%/Refract/library.json` stores games, settings and task history.
`meta-session.dpapi` stores the encrypted Meta token (sessions from the old
Electron launcher are not read; sign in again once). `launcher-backend.log` holds
backend errors. Downloads default to
`~/Downloads/Refract/<app-id>/<build-id>/`. Games' actual saves stay in the AVD.
The AVD (Android's data disk, which grows as games are installed and never
shrinks by itself) starts in `%USERPROFILE%\.android\avd`; Emulator > Storage >
Disk > Move moves it to another folder or drive and points `<avd>.ini` at it.
Five GB of free disk headroom is reserved before downloads to keep Android
bootable. Meta APIs used by community launchers are undocumented and may change;
API errors are shown rather than treating missing content as successful installs.
Meta server errors (HTTP 5xx) are retried twice, and when the version list fails
the store listing's current release is offered instead.
Library pagination is reported if Meta returns a partial entitlement response.

Opening a Meta game that is not downloaded yet checks which VR SDK its current
build uses by reading only the APK's zip directory (two ranged requests): games
on Meta's older VrApi SDK (`libvrapi.so` without an OpenXR loader) are marked
"VrApi, not supported". Downloaded and imported APKs are checked the same way.

## Verification

```powershell
npm test --prefix launcher
```

Unit tests cover Quest filtering, SSO challenge validation, DLC entitlement
selection, APK/OBB plans, download integrity/resume, unsafe paths/redirects, atomic
library persistence, shell argument handling, and the backend's stdio protocol
(settings validation, unknown methods, the Meta token never appearing in UI state,
clean shutdown). `cargo check` in `src-tauri` checks the shell.

Meta login challenge creation and installed-game scan
have been checked live. Account-specific downloads/install/DLC still need a
signed-in account test; fixture coverage is not an end-to-end Meta download test.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for RiftLift attribution.
