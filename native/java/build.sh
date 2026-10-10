#!/bin/sh
# Builds build-native/java/refract-android.jar: android.jar (de-stubbed) + firmware
# utility classes + Refract's framework implementations (src/).
set -e
HERE="$(cd "$(dirname "$0")" && pwd -W)"
ROOT="$HERE/../.."
OUT="$ROOT/build-native/java"
JDK="${REFRACT_JDK:-/c/Program Files/Java/jdk-27}"
ANDROID_JAR="${ANDROID_JAR:-$USERPROFILE/Android/Sdk/platforms/android-34/android.jar}"
SYSROOT="${REFRACT_SYSROOT:-$ROOT/../dumps/horizon-dump/fs}"
D2J="${DEX2JAR:-$ROOT/../external/dex2jar/d2j-dex2jar.bat}"
JAVA="$JDK/bin/java.exe"
JAVAC="$JDK/bin/javac.exe"
TOOL="$HERE/tools/ShimBuilder.java"
ASM="$HERE/tools/lib/asm-9.8.jar"
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
