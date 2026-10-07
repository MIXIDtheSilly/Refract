#!/usr/bin/env bash
# Linux counterpart of build_apk.ps1: builds the Refract Platform SDK stand-in (package com.oculus.horizon,
# arm64 only) into build-platform-sdk-linux/refract-platform-debug.apk. Signed with the runtime APK's key, so
# build android-runtime-apk/build_apk.sh first. Same prerequisites as that script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HERE="$ROOT/platform-sdk"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/${ANDROID_NDK_VERSION:-27.3.13750724}}"
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$SDK/build-tools/${ANDROID_BUILD_TOOLS_VERSION:-36.1.0}}"
BUILD_DIR="$ROOT/build-platform-sdk-linux"
KEYSTORE="$ROOT/build-android-runtime-linux-x86_64/debug.keystore"

java_tool() {
    if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/$1" ]]; then echo "$JAVA_HOME/bin/$1"; else command -v "$1" || echo "$1"; fi
}
JAVAC="$(java_tool javac)"
JAR="$(java_tool jar)"
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"
ANDROID_JAR="$SDK/platforms/android-29/android.jar"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"

for required in "$AAPT2" "$D8" "$ZIPALIGN" "$APKSIGNER" "$ANDROID_JAR" "$TOOLCHAIN" "$KEYSTORE"; do
    [[ -e "$required" ]] || { echo "Missing prerequisite: $required" >&2; exit 1; }
done
for required in "$JAVAC" "$JAR" cmake ninja; do
    command -v "$required" >/dev/null || { echo "Missing prerequisite: $required" >&2; exit 1; }
done

mkdir -p "$BUILD_DIR"/{classes,dex,package/lib/arm64-v8a}
cmake -S "$HERE/native" -B "$BUILD_DIR/native" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR/native"
"$AAPT2" link -I "$ANDROID_JAR" --manifest "$HERE/apk/AndroidManifest.xml" -o "$BUILD_DIR/base.apk"
mapfile -t sources < <(find "$HERE/apk/src" -name '*.java')
"$JAVAC" -source 8 -target 8 -Xlint:-options -bootclasspath "$ANDROID_JAR" -d "$BUILD_DIR/classes" "${sources[@]}"
mapfile -t classes < <(find "$BUILD_DIR/classes" -name '*.class')
"$D8" --min-api 29 --output "$BUILD_DIR/dex" "${classes[@]}"
cp "$BUILD_DIR/base.apk" "$BUILD_DIR/unsigned.apk"
cp "$BUILD_DIR/native/librefract_ovrplatform.so" "$BUILD_DIR/package/lib/arm64-v8a/"
cp "$BUILD_DIR/dex/classes.dex" "$BUILD_DIR/package/"
"$JAR" uf "$BUILD_DIR/unsigned.apk" -C "$BUILD_DIR/package" classes.dex -C "$BUILD_DIR/package" lib
"$ZIPALIGN" -f -p 4 "$BUILD_DIR/unsigned.apk" "$BUILD_DIR/aligned.apk"
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$BUILD_DIR/refract-platform-debug.apk" "$BUILD_DIR/aligned.apk"
"$APKSIGNER" verify "$BUILD_DIR/refract-platform-debug.apk"
echo "Built $BUILD_DIR/refract-platform-debug.apk"
