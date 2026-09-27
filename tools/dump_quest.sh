#!/bin/bash
# Dumps the readable parts of Horizon OS from a connected Quest (no root). Never touches /data.
# Streams one tar per partition (toybox tar skips unreadable files instead of aborting like adb pull)
# and saves the list of skipped files as <name>.denied.txt.
# Usage: dump_quest.sh [serial] [outdir]
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SERIAL="${1:-1WMHHA63M61447}"
OUT="${2:-horizon-dump}"
ADB="/c/Users/mixid/Android/Sdk/platform-tools/adb.exe -s $SERIAL"
mkdir -p "$OUT/tar"; cd "$OUT/tar" || exit 1

dump() {  # dump <name> <path relative to />
  echo "=== $2 $(date +%T)"
  $ADB exec-out "tar -cf - -C / $2 2>/data/local/tmp/tar_err.txt" > "$1.tar"
  $ADB exec-out "cat /data/local/tmp/tar_err.txt" | tr -d '\r' > "$1.denied.txt"
  echo "    $(du -h "$1.tar" | cut -f1), $(wc -l < "$1.denied.txt") unreadable"
}

for p in system system_ext product vendor odm; do dump "$p" "$p"; done
# /apex isn't listable without root; take active module names from the mount table
for a in $($ADB shell df | tr -d '\r' | grep -o '/apex/[^@ ]*@' | sed 's#/apex/##; s#@$##' | sort -u); do
  dump "apex_$a" "apex/$a"
done
$ADB shell rm -f /data/local/tmp/tar_err.txt
echo "=== DONE $(date +%T)"
