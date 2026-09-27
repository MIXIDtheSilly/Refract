# Working Yeeps performance setup

Saved 2026-09-26. This is the configuration that removed the repeatable 64 FPS plateau and that the user reported runs well. After the rename to Refract, the first build (2026-09-27) delivered about 145 frames/s with GPU sharing.

## Start it

From the repository root in PowerShell:

```powershell
.\scripts\start_emulator.ps1 -Cores 6 -Hidden
.\scripts\ensure_tsc.ps1
.\scripts\launch.ps1 -Run yeeps -TranslatorMode two-gear
```

The emulator is `refract-google-api36` on serial `emulator-5582`, with 8 GiB RAM, host GPU, Refract Vulkan GPU sharing, six vCPUs, DirectSound audio, and kernel arguments `tsc=nowatchdog idle=poll`. `ensure_tsc.ps1` checks for the fast `tsc` guest clock and reboots until it is active. Launch defaults are 1600 × 1600 pixels per eye, 109° horizontal field of view, and 250 Hz pacing ceiling (effectively uncapped). The game is `com.TrassGames.Yeeps`, activity `com.unity3d.player.UnityPlayerGameActivity`. The translator uses `berberis.mode=two-gear` and the prebuilt `prebuilts/digitalis/system/lib64/libberberis_arm64.so` (SHA-256 `B4DC5A2998C93B96C749DABB4749CDF55415EB824CEBFD5A8ADB811CA24D14FF`). The translator source change is exported as `tools/translator/digitalis-performance-fix.patch`.

Each run's logcat, viewer log and input-server log are written to `runs/<name>/` (not tracked by git).

The viewer source uses asynchronous, ordered GPU copy acknowledgments in `protocol/image_transport.cpp` and `viewer/viewer.cpp`. The viewer can receive later frames while an earlier GPU copy finishes; it sends each acknowledgment after that frame's copy query signals. Build with `viewer/build.bat`; check transport ordering with `tools/test_async_transport.bat`.

## Measured result

The old plateau was 64.5 frames/s with `image-ack-wait` averaging 9.793 ms. In the fresh run after the asynchronous viewer change and emulator restart, seven consecutive five-second windows measured 121.0, 133.3, 137.9, 133.8, 136.3, 138.0, and 138.3 frames/s. `image-ack-wait` fell to about 0.006–0.014 ms. The comparison includes the restart, so it does not isolate the exact gain from the viewer change. See [yeeps_async_results.md](yeeps_async_results.md). The game can run slower in heavier scenes; the measured rate depends on scene and camera position.

`scripts/launch.ps1` force-stops the game and current viewer before starting a new run. Avoid rerunning it while preserving an active game session.

## Unmodified APKs (no OVRPort), added 2026-09-26

- The installed Yeeps is the original, unmodified APK.
- Refract's Meta Platform SDK stand-in (`platform-sdk`, package `com.oculus.horizon`) answers the game's own `libovrplatformloader.so`.
- The emulator must report an Oculus manufacturer: `scripts\device_identity.ps1 -Identity quest` (switch back with `-Identity google` for patched APKs).
- Launch with `.\scripts\launch.ps1 -Run <name> -TranslatorMode two-gear -InstallPlatform`. Games listed in `scripts/owned_games.txt` pass the entitlement check.
- Verified: entitlement, user, and user proof OK; `questLogIntoAccount: Success`; the game joins a room; 106–115 fps in the viewer.
- Microphone and notification permissions were declined after the reinstall. Grant them with `adb shell pm grant com.TrassGames.Yeeps android.permission.RECORD_AUDIO` if wanted.
