#!/bin/bash
# Build and run the ARM64 translator host tests against the current AOSP tree.
set -eo pipefail
cd /root/aosp
export ALLOW_MISSING_DEPENDENCIES=true
export TARGET_PRODUCT=sdk_phone64_x86_64_digitalis
export TARGET_RELEASE=trunk_staging
export TARGET_BUILD_VARIANT=userdebug
source build/envsetup.sh >/dev/null
set -u
m berberis_arm64_host_tests
test_bin=out/host/linux-x86/nativetest64/berberis_arm64_host_tests/berberis_arm64_host_tests
"$test_bin" "$@"
