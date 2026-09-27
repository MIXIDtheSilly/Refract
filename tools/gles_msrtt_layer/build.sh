#!/bin/sh
# Builds librefract_gles_msrtt.so (x86_64: the emulator's GL stack, also for ARM apps under translation).
NDK=/c/Users/mixid/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin
cd "$(dirname "$0")" && "$NDK/clang++.exe" --target=x86_64-linux-android29 -O2 -fPIC -shared -std=c++17 \
  -fvisibility=hidden -static-libstdc++ -Wall -Wextra -Wno-unused-parameter \
  -o librefract_gles_msrtt.so msrtt_layer.cpp -llog -lEGL -Wl,--no-undefined
