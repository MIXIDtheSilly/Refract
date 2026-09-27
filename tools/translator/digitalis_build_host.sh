#!/bin/bash
# Rebuild the host translator library, then collect the Digitalis bundle.
cd /root/aosp || exit 1
export ALLOW_MISSING_DEPENDENCIES=true
export TARGET_PRODUCT=sdk_phone64_x86_64_digitalis TARGET_RELEASE=trunk_staging TARGET_BUILD_VARIANT=userdebug
{
  source build/envsetup.sh >/dev/null
  m libberberis_arm64
  echo HOST_EXIT=$?
  bash digitalis/scripts/build-and-package-prebuilts.sh --collect-only
  echo BUILD_EXIT=$?
} > /root/build.log 2>&1
grep -aE 'HOST_EXIT|BUILD_EXIT|^FAILED|error:' /root/build.log | head -10 | cut -c1-250
