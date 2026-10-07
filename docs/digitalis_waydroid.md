# Building Digitalis for Waydroid

Waydroid translates ARM64 games with the image's own `ndk_translation` by default. This guide builds
the Windows emulator's translator, Digitalis, for Waydroid's Android. `scripts/waydroid.sh setup
--translator digitalis` then installs it. [linux_waydroid.md](linux_waydroid.md#the-arm64-translator)
explains why Waydroid needs its own build.

| Bundle | bionic layout | Runs on |
|---|---|---|
| `prebuilts/digitalis` | Android 16 QPR0 | The Windows emulator (`BE2A.250530.026.F3`) |
| `prebuilts/digitalis-qpr2` | Android 16 QPR2 | Waydroid (LineageOS 23.2) |

## 1. Build the bundle in WSL (Windows)

The WSL AOSP tree (`/root/aosp`, Digitalis' manifest at `android16-qpr2-release`) already has
Refract's translator patches. Its `bionic` carries four local commits that give it the emulator's
QPR0 layout:

| Commit | Change |
|---|---|
| `09b13b5c` | Revert "Disable tracing during libc init." |
| `9d24db0e` | Revert "Optimize android_unsafe_frame_pointer_chase" |
| `9224785c` | Revert "bionic: Optimize TLS memory by isolating per-thread libgen buffer" |
| `b862b505` | Use `__builtin_align_up` after libgen revert |

`tools/translator/digitalis_build_qpr2.sh` builds the bundle from `bionic` without those commits (the
synced `f22516cb`). It then restores them and rebuilds, so the Windows workflow
(`digitalis_build_host.sh`, `digitalis_deploy.sh`) and `prebuilts/digitalis` are unchanged.

1. Get this branch into the Windows checkout (`Documents\QuestOnPC\Refract`).
2. In PowerShell, run:

   ```powershell
   wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/Users/mixid/Documents/QuestOnPC/Refract/tools/translator/digitalis_build_qpr2.sh
   ```

The script:

1. Checks that `bionic` has no uncommitted changes and is at `b862b505` (or `f22516cb`). It stops
   otherwise.
2. Saves the four commits as the branch `refract-qpr0-layout`, then checks out `f22516cb`.
3. Runs `digitalis/scripts/build-and-package-prebuilts.sh --full`. The log is
   `/root/digitalis-qpr2-build.log`.
4. Copies the bundle to `C:\Users\mixid\Documents\digitalis-qpr2-prebuilts.tar.gz`. Give another
   path, as seen from WSL, as its first argument.
5. Checks out `refract-qpr0-layout` again and rebuilds. The log is
   `/root/digitalis-qpr2-build.log.restore`.

It returns `bionic` to the QPR0 layout even if the build fails. Both builds are incremental full
builds of the tree: `bionic` and the system image are rebuilt.

## 2. Install it on Linux

Mount the Windows drive (read-only is enough). From the Linux checkout, run:

```sh
tarball=/run/media/$USER/Acer/Users/mixid/Documents/digitalis-qpr2-prebuilts.tar.gz
mkdir -p prebuilts/digitalis-qpr2
tar -xzf "$tarball" -C prebuilts/digitalis-qpr2 --strip-components=1
(cd prebuilts/digitalis-qpr2 && sha256sum -c --quiet SHA256SUMS)
python3 tools/translator/digitalis_bionic_layout.py prebuilts/digitalis-qpr2 --expect qpr2
```

The last command must print `qpr2 (bionic_tls at 0x300)`. If it prints `qpr0`, the tree was built
with the layout commits; don't install that bundle on Waydroid.

Then install it (this restarts Waydroid's container):

```sh
sudo scripts/waydroid.sh setup --translator digitalis
scripts/waydroid.sh check        # ARM64 translator: Digitalis
```

`sudo scripts/waydroid.sh setup` (or `--translator ndk`) switches back to `ndk_translation`. It
removes Digitalis' files from the overlay.

## If ARM64 games crash at start

Every ARM64 program crashing at fault address `0x2fa1` (`HandleFatalSignal: sig=11
si_addr=0x2fa1` in the log) means the bundle's layout doesn't match the image. Check it with
`digitalis_bionic_layout.py`, as in step 2.
