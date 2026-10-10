#!/bin/sh
# Builds the ARM64 guest side: HLE stub libraries and the refract_app launcher,
# into build-native/guest/{lib64,bin}.
set -e
NDK=${NDK:-$USERPROFILE/Android/Sdk/ndk/27.3.13750724}
BIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
CC="$BIN/clang.exe --target=aarch64-linux-android29"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../../build-native/guest"
mkdir -p "$OUT/lib64" "$OUT/bin"
python "$HERE/../tools/gen_thunks.py"
for src in "$HERE"/gen/*.S; do
  name="$(basename "${src%.S}").so"
  map="${src%.S}.map"
  extra=""
  [ -f "$map" ] && extra="-Wl,--version-script,$map -Wl,--undefined-version"
  $CC -shared -nostdlib -fPIC -Wl,-soname,"$name" -Wl,--hash-style=both -Wl,-z,max-page-size=4096 $extra \
      -o "$OUT/lib64/$name" "$src"
done
$CC -O2 -g -fPIE -pie -o "$OUT/bin/refract_app" "$HERE/refract_app.c" \
    -L"$OUT/lib64" -lrefract_host -lrefract_jni -ldl
ls "$OUT/lib64" | tr '\n' ' '; echo
