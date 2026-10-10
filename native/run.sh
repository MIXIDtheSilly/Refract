#!/bin/sh
# Runs a guest program under refract_native with the Quest 2 dump as sysroot.
# usage: native/run.sh [refract_native options] -- /guest/path args...
export MSYS_NO_PATHCONV=1
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
QUEST="$(cd "$ROOT/.." && pwd -W)"
exec "$ROOT/build-native/refract_native.exe" \
  --sysroot "${REFRACT_SYSROOT:-$QUEST/dumps/horizon-dump/fs}" \
  --mount "/data/local/tmp/t=$ROOT/native/tests/guest/out" "$@"
