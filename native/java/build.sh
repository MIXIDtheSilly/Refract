#!/bin/sh
# Builds build-native/java/refract-android.jar: android.jar (de-stubbed) + firmware
# utility classes + Refract's framework implementations (src/).
set -e
HERE="$(cd "$(dirname "$0")" && pwd -W)"
ROOT="$HERE/../.."
OUT="$ROOT/build-native/java"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [ -z "$SDK" ]; then
  SDK="${LOCALAPPDATA:-$USERPROFILE}/Android/Sdk"
fi
SDK="$(cygpath -m "$SDK")"
JDK="$(cygpath -m "${REFRACT_JDK:-$ROOT/jdk}")"
ANDROID_JAR="$(cygpath -m "${ANDROID_JAR:-$SDK/platforms/android-34/android.jar}")"
SYSROOT="$(cygpath -m "${REFRACT_SYSROOT:-$ROOT/build-native/sysroot}")"
D2J="${DEX2JAR:-$ROOT/../external/dex2jar/d2j-dex2jar.bat}"
D2J="$(cygpath -m "$D2J")"
JAVA="$JDK/bin/java.exe"
JAVAC="$JDK/bin/javac.exe"
TOOL="$HERE/tools/ShimBuilder.java"
ASM="$HERE/tools/lib/asm-9.8.jar"
for required in "$JAVA" "$JAVAC" "$ANDROID_JAR" "$D2J" "$SYSROOT/system/framework/framework.jar" "$SYSROOT/apex/com.android.art/javalib/core-libart.jar"; do
  if [ ! -f "$required" ]; then
    echo "Required Java shim build input not found: $required" >&2
    exit 1
  fi
done
mkdir -p "$OUT/firmware"

if [ ! -f "$OUT/android-base.jar" ] || [ "$TOOL" -nt "$OUT/android-base.jar" ]; then
  "$JAVA" -cp "$ASM" "$TOOL" base "$ANDROID_JAR" "$OUT/android-base.jar"
fi
for jar in system/framework/framework.jar apex/com.android.art/javalib/core-libart.jar; do
  name="$(basename "$jar")"
  if [ ! -f "$OUT/firmware/$name" ]; then
    "$D2J" -f -o "$OUT/firmware/$name" "$SYSROOT/$jar" >/dev/null
  fi
done
"$JAVA" -cp "$ASM" "$TOOL" real "$HERE/real-classes.txt" "$OUT/android-real.jar" \
  "$OUT/firmware/framework.jar" "$OUT/firmware/core-libart.jar"

rm -rf "$OUT/overlay-classes"
mkdir -p "$OUT/overlay-classes"
find "$HERE/src" -name '*.java' > "$OUT/sources.txt"
"$JAVAC" -nowarn -encoding UTF-8 --release 21 -proc:none -XDstringConcat=inline \
  -cp "$OUT/android-real.jar;$OUT/android-base.jar" -d "$OUT/overlay-classes" "@$OUT/sources.txt"
"$JAVA" -cp "$ASM" "$TOOL" merge "$OUT/android-base.jar" "$OUT/android-real.jar" "$OUT/overlay-classes" "$OUT/refract-android.jar"
