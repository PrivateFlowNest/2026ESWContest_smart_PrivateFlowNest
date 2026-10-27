#!/bin/sh
# bundle_libs.sh <bin dir> <build dir>: copy every NEEDED library of the binaries in <bin dir> that was
# built in <build dir> (transitively), strip, and set RUNPATH=$ORIGIN so the folder is relocatable.
set -eu
BIN="$1"; BUILD="$2"
needed() { aarch64-linux-gnu-readelf -d "$1" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  for f in "$BIN"/*; do
    for lib in $(needed "$f"); do
      [ -e "$BIN/$lib" ] && continue
      src=$(find "$BUILD" -name "$lib" -type f | head -1)
      if [ -n "$src" ]; then cp "$src" "$BIN/"; changed=1; fi
    done
  done
done
for f in "$BIN"/*; do
  aarch64-linux-gnu-strip "$f"
  patchelf --set-rpath '$ORIGIN' "$f"
done
