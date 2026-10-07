#!/bin/bash
# Builds a Digitalis bundle for Waydroid (scripts/waydroid.sh setup --translator digitalis) in the WSL AOSP tree.
#
# Digitalis' ARM64 bionic shares the Android host's pthread_internal_t and bionic_tls, so it must be built with the
# same bionic as the Android it runs on. The tree is Android 16 QPR2 (BP4A), with four local bionic commits that give
# the guest bionic the Windows emulator's QPR0 (BE2A) layout: the reverts of "Disable tracing during libc init.",
# "Optimize android_unsafe_frame_pointer_chase" (stack_bottom) and the libgen buffer TLS change, plus a build fix.
# Waydroid's LineageOS 23.2 is QPR2 itself, so this builds the bundle from bionic without them, then puts them back
# and rebuilds, so the tree and out/ are as the Windows workflow (digitalis_build_host.sh) expects.
#
# usage (in WSL, as root): digitalis_build_qpr2.sh [OUTPUT.tar.gz]
#   default output: /mnt/c/Users/$WINDOWS_USER/Documents/digitalis-qpr2-prebuilts.tar.gz
set -eo pipefail
QPR2=f22516cbc67e81c13cf943ce02f01b8929a98726     # bionic as synced (android16-qpr2-release).
QPR0_LAYOUT=b862b505d170028a5923c7cb96edf89420492239  # The same plus the four layout commits.
output="${1:-/mnt/c/Users/${WINDOWS_USER:-mixid}/Documents/digitalis-qpr2-prebuilts.tar.gz}"
log=/root/digitalis-qpr2-build.log
cd /root/aosp
export ALLOW_MISSING_DEPENDENCIES=true
export TARGET_PRODUCT=sdk_phone64_x86_64_digitalis TARGET_RELEASE=trunk_staging TARGET_BUILD_VARIANT=userdebug

[[ -z "$(git -C bionic status --porcelain --untracked-files=no)" ]] || { echo "bionic has uncommitted changes; commit or stash them first" >&2; exit 1; }
head="$(git -C bionic rev-parse HEAD)"
[[ "$head" == "$QPR0_LAYOUT" || "$head" == "$QPR2" ]] || { echo "bionic is at $head, not the expected $QPR0_LAYOUT" >&2; exit 1; }
git -C bionic branch -f refract-qpr0-layout "$QPR0_LAYOUT"  # Keeps the layout commits reachable.

restore() {
    git -C bionic checkout -q --detach refract-qpr0-layout
    echo "bionic is back at the Windows (QPR0) layout: $(git -C bionic log -1 --format='%h %s')"
}
trap restore EXIT

git -C bionic checkout -q --detach "$QPR2"
echo "Building Digitalis with stock QPR2 bionic (log: $log); this rebuilds bionic and the system image."
bash digitalis/scripts/build-and-package-prebuilts.sh --full >"$log" 2>&1 || { tail -n 30 "$log"; exit 1; }
bundle="$(ls -t digitalis/dist/*.tar.gz | head -n1)"
cp "$bundle" "$output"
echo "Waydroid bundle: $output ($(sha256sum "$output" | cut -d' ' -f1))"

restore
trap - EXIT
echo "Rebuilding the Windows (QPR0 layout) build in out/ (log: $log.restore)."
bash digitalis/scripts/build-and-package-prebuilts.sh --full >"$log.restore" 2>&1 || { tail -n 30 "$log.restore"; exit 1; }
echo "Done. prebuilts/digitalis (Windows) is unchanged; out/ matches it again."
