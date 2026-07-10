#!/usr/bin/env bash
# extract.sh — extract hardware firmware from a reMarkable production image
#
# Usage:
#   ./extract.sh <image.ext4[.verity][.gz]> [output-dir]
#
# output-dir defaults to the directory this script lives in (firmware/).
# Requires: sudo (for loop-mount), gunzip, sha256sum.
#
# The input image is the publicly distributed production ext4 rootfs image,
# e.g. remarkable-production-image-3.27.1.0-chiappa-public.ext4.verity.gz
# These are version-agnostic paths: reMarkable has kept the internal layout
# stable across firmware generations.
set -euo pipefail

IMAGE="${1:-}"
OUTDIR="${2:-$(cd "$(dirname "$0")" && pwd)}"

if [ -z "$IMAGE" ]; then
    echo "Usage: $0 <image.ext4[.verity][.gz]> [output-dir]" >&2
    exit 1
fi
[ -f "$IMAGE" ] || { echo "Not found: $IMAGE" >&2; exit 1; }

TMPDIR_MOUNT=$(mktemp -d)
TMPIMG=""

cleanup() {
    sudo umount "$TMPDIR_MOUNT" 2>/dev/null || true
    rmdir "$TMPDIR_MOUNT" 2>/dev/null || true
    [ -n "$TMPIMG" ] && rm -f "$TMPIMG"
}
trap cleanup EXIT

# Decompress if gzipped
case "$IMAGE" in
    *.gz)
        echo "==> decompressing $IMAGE ..."
        TMPIMG=$(mktemp --suffix=.ext4)
        gunzip -c "$IMAGE" > "$TMPIMG"
        LOOPIMG="$TMPIMG"
        ;;
    *)
        LOOPIMG="$IMAGE"
        ;;
esac

echo "==> mounting ext4 (read-only) ..."
# The .verity suffix means verity metadata may be appended; the ext4 driver
# reads the superblock and ignores trailing data, so plain mount works fine.
sudo mount -o ro,loop "$LOOPIMG" "$TMPDIR_MOUNT"

extract() {
    local src="$1" dst="$2"
    local full="$TMPDIR_MOUNT/$src"
    if [ -e "$full" ]; then
        mkdir -p "$(dirname "$OUTDIR/$dst")"
        cp "$full" "$OUTDIR/$dst"
        echo "  extracted: $dst"
    else
        echo "  MISSING (skipping): $src" >&2
    fi
}

extract_glob() {
    local pattern="$1" dstdir="$2"
    mkdir -p "$OUTDIR/$dstdir"
    local found=0
    for f in $TMPDIR_MOUNT/$pattern; do
        [ -e "$f" ] || continue
        cp "$f" "$OUTDIR/$dstdir/"
        echo "  extracted: $dstdir/$(basename "$f")"
        found=1
    done
    [ "$found" -eq 1 ] || echo "  MISSING (skipping): $pattern" >&2
}

echo "==> extracting kernel firmware blobs ..."
extract lib/firmware/ctn730.fw              lib/firmware/ctn730.fw
extract lib/firmware/elants_spi.bin         lib/firmware/elants_spi.bin
extract lib/firmware/marker-asic.bin        lib/firmware/marker-asic.bin
extract lib/firmware/marker-mcu.bin         lib/firmware/marker-mcu.bin
extract lib/firmware/nxp/rgpower.bin        lib/firmware/nxp/rgpower.bin
extract lib/firmware/nxp/sd_w61x_v1.bin.se lib/firmware/nxp/sd_w61x_v1.bin.se
extract lib/firmware/nxp/uartspi_n61x_v1.bin.se lib/firmware/nxp/uartspi_n61x_v1.bin.se
extract lib/firmware/nxp/WlanCalData_ext.conf   lib/firmware/nxp/WlanCalData_ext.conf
extract lib/firmware/regulatory.db          lib/firmware/regulatory.db
extract lib/firmware/regulatory.db.p7s      lib/firmware/regulatory.db.p7s
extract lib/firmware/tee.bin                lib/firmware/tee.bin
extract lib/firmware/tee.elf                lib/firmware/tee.elf
extract lib/firmware/tee-header_v2.bin      lib/firmware/tee-header_v2.bin
extract lib/firmware/tee-pageable_v2.bin    lib/firmware/tee-pageable_v2.bin
extract lib/firmware/tee-pager_v2.bin       lib/firmware/tee-pager_v2.bin
extract lib/firmware/tee-raw.bin            lib/firmware/tee-raw.bin
extract_glob "lib/firmware/spld_rev_*.hex"  lib/firmware

echo "==> extracting e-ink waveforms ..."
extract_glob "usr/share/remarkable/*.eink"          waveforms
extract_glob "usr/share/remarkable/colortable_*.bin" waveforms
extract_glob "usr/share/remarkable/ct33_*.bin"       waveforms

echo "==> extracting Qt6/epaper vendor bundle (for einkbridge) ..."
# The bundle is a self-contained glibc+Qt6+libqsgepaper tree used by einkbridge.
# It lives under /usr/share/remarkable/qtbase/ or similar in older firmware;
# in 3.27+ it may be at a different path.  We copy the whole candidate dirs.
for candidate in \
    usr/lib/libqsgepaper.so \
    usr/lib/libqsgepaper.so.1 \
    usr/share/remarkable/libqsgepaper.so
do
    [ -e "$TMPDIR_MOUNT/$candidate" ] || continue
    echo "  found libqsgepaper at: $candidate"
    extract "$candidate" "bundle/$(basename "$candidate")"
done
echo "  (full Qt6 bundle extraction: see eink/scripts/build-bundle.sh)"

echo "==> verifying checksums against MANIFEST.md ..."
if command -v sha256sum >/dev/null 2>&1 && [ -f "$OUTDIR/MANIFEST.md" ]; then
    FAIL=0
    while IFS= read -r line; do
        hash=$(echo "$line" | awk '{print $1}')
        file=$(echo "$line" | awk '{print $2}')
        [ ${#hash} -eq 64 ] || continue
        [ -f "$OUTDIR/$file" ] || { echo "  MISSING: $file" >&2; FAIL=1; continue; }
        actual=$(sha256sum "$OUTDIR/$file" | awk '{print $1}')
        if [ "$actual" = "$hash" ]; then
            echo "  OK: $file"
        else
            echo "  MISMATCH: $file (expected $hash, got $actual)" >&2
            FAIL=1
        fi
    done < <(grep -E '^[a-f0-9]{64}' "$OUTDIR/MANIFEST.md")
    [ "$FAIL" -eq 0 ] && echo "All checksums OK." || echo "WARNING: some files failed checksum." >&2
else
    echo "  (skipping — sha256sum or MANIFEST.md not found)"
fi

echo "==> done. Files written to: $OUTDIR"
