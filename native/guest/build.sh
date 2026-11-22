#!/bin/sh
# Builds ARM64 guest compatibility libraries and the refract_app launcher,
# into build-native/guest/{lib64,bin}.
set -e
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$SDK" ]; then
  if [ -n "${LOCALAPPDATA:-}" ]; then SDK="$LOCALAPPDATA/Android/Sdk"; else SDK="$HOME/Android/Sdk"; fi
fi
SDK="$(cygpath -u "$SDK")"
NDK=${NDK:-$SDK/ndk/27.3.13750724}
if [ ! -d "$NDK" ]; then NDK="$(cygpath -u "$NDK")"; fi
BIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
if [ ! -f "$BIN/clang.exe" ]; then
  echo "Android NDK compiler not found: $BIN/clang.exe" >&2
  exit 1
fi
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
$CC -shared -nostdlib -fPIC -O2 -Wl,-soname,libossdk.oculus.so -Wl,--hash-style=both -Wl,-z,max-page-size=4096 \
    -o "$OUT/lib64/libossdk.oculus.so" "$HERE/ossdk_stub.c"
$CC -O2 -g -fPIE -pie -o "$OUT/bin/refract_app" "$HERE/refract_app.c" \
    -L"$OUT/lib64" -lrefract_host -lrefract_jni -ldl
ls "$OUT/lib64" | tr '\n' ' '; echo
