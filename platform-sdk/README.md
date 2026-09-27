# Meta Platform SDK stand-in

Lets unmodified Quest APKs initialise the Meta Platform SDK (`libovrplatformloader.so`) under Refract, so they no longer need OVRPort patching.

## How the game finds it

Each game bundles Meta's `libovrplatformloader.so`. At `ovr_*Initialize*` it:

1. Looks for the package `com.oculus.platformsdkruntime`, which must be a system app. If that's missing, it uses `com.oculus.horizon`.
2. Calls `createPackageContext(pkg, INCLUDE_CODE | IGNORE_SECURITY)` and loads `com.oculus.platform.loader.EntryPoint` from that package.
3. Calls `static long load64(Context app, Context pkg, int 1, int 1, int 108, int 0, int 0)`. It uses the result as `void* getProc(const char* name)` to resolve every `ovr_*` function.

If the package isn't found, the loader aborts with "DisplayErrorAndExit: Failed to launch SystemActivities".

This APK is `com.oculus.horizon`. Its `EntryPoint` loads `librefract_ovrplatform.so`, which is arm64 because it runs inside the game's process, and returns its resolver.

- `native/ovr_platform.cpp` implements, by hand:
  - init, the message queue, users, the access token and user proof, entitlement, and app version and launch details.
- Every other name resolves to a stub from `native/stub_pool.S`. The stub logs its name on the first call and returns something harmless:
  - `0`
  - `""` for string getters
  - an empty array for `ovr_Message_Get*Array`
  - a message for request functions: an error by default, or an empty success for the social, achievement and presence calls in `kSucceedingRequests`.
- `native/ovr_tables.h` maps request functions to message types and lists the string getters. It was generated from a Unity game's `Oculus.Platform` IL2CPP metadata (SDK 1.1.108); message type values are stable hashes.

## Configuration (system properties, `debug.refract.platform.*`)

| Property | Default |
| --- | --- |
| `owned.<package>` | unset. Only `1` makes the entitlement check succeed. `scripts/launch.ps1` sets it for each package in `scripts/owned_games.txt`. |
| `user_id` | derived from the device's ANDROID_ID |
| `user_name` / `display_name` | `Refract` |
| `access_token` | `Refract<user id>` |
| `verbose` | `0`. `1` logs every resolved name and message (`launch.ps1 -PlatformVerbose`). |
| `mic_selftest` | `0`. `1` records 2 s through `ovr_Microphone_*` when a game loads the library and logs the levels. |

`ovr_Microphone_*` captures 48 kHz mono float through AAudio from the default input. It needs the game's RECORD_AUDIO permission. On the emulator, that input is the Windows default recording device, and only when the emulator runs with `-allow-host-audio` (`start_emulator.ps1` passes it; `-NoHostMic` turns it off).

In-app purchases and DLC are never granted: purchase lists are empty, and checkout and consume return errors.

## Build and install

```powershell
.\platform-sdk\build_apk.ps1 -Sdk C:\Users\mixid\Android\Sdk -Jdk 'C:\Program Files\Java\jdk-27'
.\scripts\launch.ps1 -Run X -InstallPlatform
```

Visibility: games that query `MAIN` intents, as Quest games do, see the package through its no-op `VisibilityActivity`. Other games need the `--force-queryable` install, which `-InstallPlatform` does; that lasts only until the next reboot.

Unity's `OculusUnity.getIsOnOculusHardware()` also requires `Build.MANUFACTURER` to contain "oculus". Switch the emulator with `scripts\device_identity.ps1 -Identity quest` (or `google` for OVRPort-patched APKs).

Logcat tags: `Refract-OVRPlatform` (this library) and `OVRPlatform-Loader` (the game's loader).
