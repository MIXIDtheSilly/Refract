# Refract launcher

A native Windows desktop frontend for the Refract runtime: your installed Android
games, the live Quest storefront, owned-game downloads, expansion files and DLC.

## Run

Requires Windows 11 (WebView2 is built in), Node.js 22+, Python, Rust from
[rustup.rs](https://rustup.rs) with the Visual Studio C++ build tools, and the
project's existing Android SDK/Windows Refract setup. From the project root:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_launcher.ps1
```

or double-click **Refract Launcher.cmd**. The first run installs the locked npm
dependencies and builds `src-tauri/target/release/refract-launcher.exe` (a few
minutes); later runs rebuild only when the UI or shell sources changed.

## How it is put together

- `src/` is the interface (React, plain CSS, the Jost typeface). Gray surfaces,
  white controls and the logo from `img/Refract_logo_trans.svg`.
- `src-tauri/` is the native shell (Rust, Tauri 2). It owns the window, file
  dialogs, the Meta sign-in window, opening folders/links and the encrypted Meta
  session. The UI has one command, `call`, and only the main window may use it.
- `backend/server.mjs` holds the launcher logic (library, Meta store, downloads,
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
- **Store:** Search the live Meta catalog; open a listing and add it to the
  library. Purchases open on Meta's site. Sign in to list your Quest entitlements
  and access downloads; a Rift purchase is not a Quest entitlement.
- **Connect Meta:** Sign in on Meta's hosted page, in a separate private window
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
- **Play:** Calls `tools/run_windows_game.ps1`, preserving automatic OpenXR eye
  resolution, SteamVR name/icon, GPU texture sharing, and the save-aware shutdown.
  Closing the game preview stops the game. Closing the launcher does not
  intentionally stop a running game; stop it with its preview window.

The current default AVD is `refract-google-api36` on port 5580 with 8 GB guest RAM. It must be an
Android 16 (API 36) AVD: games run on the Digitalis ARM64 translator, which is built for Android 16.
Starting the emulator installs Digitalis into it if needed (`scripts/translator.ps1`).
Change these in Settings for another existing Refract setup. The launcher does not
provision WHPX, SteamVR, the system image, or an AVD from scratch.

## Data and limitations

`%APPDATA%/Refract/library.json` stores games, settings and task history.
`meta-session.dpapi` stores the encrypted Meta token (sessions from the old
Electron launcher are not read; sign in again once). `launcher-backend.log` holds
backend errors. Downloads default to
`~/Downloads/Refract/<app-id>/<build-id>/`. Games' actual saves stay in the AVD.
Five GB of free disk headroom is reserved before downloads to keep Android
bootable. Meta APIs used by community launchers are undocumented and may change;
API errors are shown rather than treating missing content as successful installs.
Library pagination is reported if Meta returns a partial entitlement response;
additional owned apps can be added from the store.

## Verification

```powershell
npm test --prefix launcher
```

Unit tests cover Quest filtering, SSO challenge validation, DLC entitlement
selection, APK/OBB plans, download integrity/resume, unsafe paths/redirects, atomic
library persistence, shell argument handling, and the backend's stdio protocol
(settings validation, unknown methods, the Meta token never appearing in UI state,
clean shutdown). `cargo check` in `src-tauri` checks the shell.

Meta login challenge creation, public storefront search and installed-game scan
have been checked live. Account-specific downloads/install/DLC still need a
signed-in account test; fixture coverage is not an end-to-end Meta download test.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for RiftLift attribution.
