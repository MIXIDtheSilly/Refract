#!/bin/sh
# Builds the ARM64 guest test programs with the NDK.
set -e
NDK=${NDK:-$USERPROFILE/Android/Sdk/ndk/27.3.13750724}
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe --target=aarch64-linux-android29"
cd "$(dirname "$0")"
mkdir -p out
for src in *.c; do
  $CC -O2 -g -o "out/${src%.c}" "$src" -llog -lvulkan
done
ls out
