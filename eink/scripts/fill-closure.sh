#!/usr/bin/env bash
# Iteratively copy any NEEDED soname (referenced by any .so in the bundle) that
# is missing from lib/, pulling from the vendor rootfs, until the closure is
# fixed-point complete. Also recreates the .so.6 soname symlinks.
set -euo pipefail
VROOT=${VROOT:-/mnt/vroot}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO/eink/bundle/eink
SRCDIRS=("$VROOT/usr/lib" "$VROOT/lib")

find_src() { for d in "${SRCDIRS[@]}"; do [ -e "$d/$1" ] && { readlink -f "$d/$1"; return; }; done; }

pass=0
while :; do
  pass=$((pass+1))
  have=$(ls "$OUT/lib/")
  added=0
  while read -r so; do
    for n in $(readelf -d "$so" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}'); do
      echo "$have" | grep -qx "$n" && continue
      src=$(find_src "$n")
      if [ -z "$src" ]; then echo "  UNRESOLVABLE: $n"; continue; fi
      cp -a "$src" "$OUT/lib/"
      # symlink the requested soname -> realfile
      ln -sf "$(basename "$src")" "$OUT/lib/$n"
      echo "  + $n -> $(basename "$src")"
      added=$((added+1))
    done
  done < <(find "$OUT" -name '*.so' -o -name '*.so.*' | grep -v "$OUT/lib/")
  echo "pass $pass: added $added"
  [ "$added" -eq 0 ] && break
done
echo "== fixed point reached; verifying =="
have=$(ls "$OUT/lib/")
miss=0
while read -r so; do
  for n in $(readelf -d "$so" 2>/dev/null | awk -F'[][]' '/NEEDED/{print $2}'); do
    echo "$have" | grep -qx "$n" || { echo "STILL MISSING: $n ($so)"; miss=$((miss+1)); }
  done
done < <(find "$OUT" -name '*.so' -o -name '*.so.*')
echo "still-missing: $miss"
du -sh "$OUT"