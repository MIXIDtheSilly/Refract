#!/usr/bin/env bash
# Linux counterpart of build_apk.ps1: builds the Refract OpenXR runtime APK and the com.oculus.systemdriver
# stand-in into build-android-runtime-linux-<abi>/. ARM64-only games (translated by Digitalis) load the
# arm64-v8a runtime; x86_64 samples load the x86_64 one.
#   ANDROID_ABI=arm64-v8a android-runtime-apk/build_apk.sh
# Needs the Android SDK (ANDROID_HOME, default ~/Android/Sdk) with build-tools, platform android-29 and the
# NDK, CMake, Ninja and a JDK (javac, jar, keytool on PATH or under JAVA_HOME).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$ROOT/android-runtime-apk"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/${ANDROID_NDK_VERSION:-27.3.13750724}}"
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$SDK/build-tools/${ANDROID_BUILD_TOOLS_VERSION:-36.1.0}}"
ABI="${ANDROID_ABI:-x86_64}"
BUILD_DIR="$ROOT/build-android-runtime-linux-$ABI"
# ABI variants replace the same package and must use the same signing identity.
KEYSTORE="$ROOT/build-android-runtime-linux-x86_64/debug.keystore"

case "$ABI" in
    x86_64) TARGET=x86_64-linux-android29 ;;
    arm64-v8a) TARGET=aarch64-linux-android29 ;;
    *) echo "ANDROID_ABI must be x86_64 or arm64-v8a" >&2; exit 2 ;;
esac

java_tool() {
    if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/$1" ]]; then echo "$JAVA_HOME/bin/$1"; else command -v "$1" || echo "$1"; fi
}
JAVAC="$(java_tool javac)"
JAR="$(java_tool jar)"
KEYTOOL="$(java_tool keytool)"
AAPT2="$BUILD_TOOLS/aapt2"
D8="$BUILD_TOOLS/d8"
ZIPALIGN="$BUILD_TOOLS/zipalign"
APKSIGNER="$BUILD_TOOLS/apksigner"
ANDROID_JAR="$SDK/platforms/android-29/android.jar"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"

for required in "$AAPT2" "$D8" "$ZIPALIGN" "$APKSIGNER" "$ANDROID_JAR" "$TOOLCHAIN" "$CLANG"; do
    [[ -e "$required" ]] || { echo "Missing prerequisite: $required" >&2; exit 1; }
done
for required in "$JAVAC" "$JAR" "$KEYTOOL" cmake ninja; do
    command -v "$required" >/dev/null || { echo "Missing prerequisite: $required (install a JDK, CMake and Ninja)" >&2; exit 1; }
done

mkdir -p "$BUILD_DIR"/{res,gen,classes,dex,"package/lib/$ABI"}
cmake -S "$ROOT" -B "$BUILD_DIR/runtime" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release \
    -DREFRACT_BUILD_HOST_BRIDGE=OFF \
    -DREFRACT_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR/runtime"

"$AAPT2" compile --dir "$APP_DIR/res" -o "$BUILD_DIR/res"
"$AAPT2" link -I "$ANDROID_JAR" --manifest "$APP_DIR/AndroidManifest.xml" --java "$BUILD_DIR/gen" \
    -o "$BUILD_DIR/base.apk" "$BUILD_DIR"/res/*.flat
mapfile -t sources < <(find "$APP_DIR/src" "$BUILD_DIR/gen" -name '*.java')
"$JAVAC" -source 8 -target 8 -Xlint:-options -bootclasspath "$ANDROID_JAR" -d "$BUILD_DIR/classes" "${sources[@]}"
mapfile -t classes < <(find "$BUILD_DIR/classes" -name '*.class')
"$D8" --min-api 29 --output "$BUILD_DIR/dex" "${classes[@]}"
cp "$BUILD_DIR/base.apk" "$BUILD_DIR/unsigned.apk"
cp "$BUILD_DIR/runtime/android-runtime/libopenxr_runtime.so" "$BUILD_DIR/package/lib/$ABI/"
cp "$BUILD_DIR/dex/classes.dex" "$BUILD_DIR/package/"
"$JAR" uf "$BUILD_DIR/unsigned.apk" -C "$BUILD_DIR/package" classes.dex -C "$BUILD_DIR/package" lib
"$ZIPALIGN" -f -p 4 "$BUILD_DIR/unsigned.apk" "$BUILD_DIR/aligned.apk"
if [[ ! -f "$KEYSTORE" ]]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    "$KEYTOOL" -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$BUILD_DIR/refract-openxr-runtime-debug.apk" "$BUILD_DIR/aligned.apk"
"$APKSIGNER" verify "$BUILD_DIR/refract-openxr-runtime-debug.apk"
echo "Built $BUILD_DIR/refract-openxr-runtime-debug.apk"

# The com.oculus.systemdriver stand-in (systemdriver/AndroidManifest.xml): the same runtime library for games
# whose Meta OpenXR loader looks for it there. Signed with the same key.
DRIVER="$BUILD_DIR/systemdriver"
mkdir -p "$DRIVER"/{classes,dex,"package/lib/$ABI"}
"$AAPT2" link -I "$ANDROID_JAR" --manifest "$APP_DIR/systemdriver/AndroidManifest.xml" -o "$DRIVER/unsigned.apk"
mapfile -t sources < <(find "$APP_DIR/systemdriver/src" -name '*.java')
"$JAVAC" -source 8 -target 8 -Xlint:-options -bootclasspath "$ANDROID_JAR" -d "$DRIVER/classes" "${sources[@]}"
mapfile -t classes < <(find "$DRIVER/classes" -name '*.class')
"$D8" --min-api 29 --output "$DRIVER/dex" "${classes[@]}"
"$CLANG" --target="$TARGET" -shared -fPIC -O2 -Wall -o "$DRIVER/package/lib/$ABI/librefract_driver.so" \
    "$APP_DIR/systemdriver/refract_driver.c" -llog -ldl
cp "$BUILD_DIR/runtime/android-runtime/libopenxr_runtime.so" "$DRIVER/package/lib/$ABI/"
cp "$DRIVER/dex/classes.dex" "$DRIVER/package/"
"$JAR" uf "$DRIVER/unsigned.apk" -C "$DRIVER/package" classes.dex -C "$DRIVER/package" lib
"$ZIPALIGN" -f -p 4 "$DRIVER/unsigned.apk" "$DRIVER/aligned.apk"
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$BUILD_DIR/refract-systemdriver-debug.apk" "$DRIVER/aligned.apk"
"$APKSIGNER" verify "$BUILD_DIR/refract-systemdriver-debug.apk"
echo "Built $BUILD_DIR/refract-systemdriver-debug.apk"
