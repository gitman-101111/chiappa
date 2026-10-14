#!/usr/bin/env bash
# Extract firmware + e-ink waveform data from a vendor rootfs image (read-only).
# Produces:  <out>/firmware/   (the /lib/firmware tree)
#            <out>/waveforms/  (the /usr/share/remarkable tree: *.eink, colortable, ct33)
#            <out>/boot/       (vendor fitImage.ahab + the 8 KB AHAB header)
#
# Usage: sudo ./extract-vendor-data.sh <vendor-rootfs.img> [outdir]
#   <vendor-rootfs.img> = a mountable ext4 image. If you have the recovery
#   '*.ext4.verity.gz', first: gunzip -k it. dm-verity images usually still mount
#   read-only (the verity hash tree is appended/ignored for a plain ro mount); if
#   your kernel refuses, truncate to the ext4 size or use 'debugfs' to dump files.
set -euo pipefail

IMG="${1:?usage: $0 <vendor-rootfs.img> [outdir]}"
OUT="${2:-./out}"
MNT="$(mktemp -d)"

mkdir -p "$OUT/firmware" "$OUT/waveforms" "$OUT/boot"
cleanup(){ mountpoint -q "$MNT" && umount "$MNT"; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT

# Take the 8 KB AHAB header off a vendor fitImage.ahab ([8 KB AHAB header][FIT]);
# it is reused verbatim when building your own FIT (docs/fitimage.md).
extract_boot(){
  local fit="/boot/vendor-fitImage.ahab"
  [[ -s "" ]] || { echo "!! no fitImage.ahab extracted; skipping header"; return 0; }
  dd if="" of="/boot/ahab-header.bin" bs=8192 count=1 status=none
  echo ">> AHAB header -> /boot/ahab-header.bin"
}

echo ">> mounting $IMG read-only…"
if ! mount -o ro,loop "$IMG" "$MNT" 2>/dev/null; then
  echo "!! direct mount failed (likely dm-verity tail). Falling back to debugfs dump."
  command -v debugfs >/dev/null || { echo "install e2fsprogs (debugfs)"; exit 1; }
  debugfs -R "rdump /lib/firmware $OUT/firmware" "$IMG" || true
  debugfs -R "rdump /usr/share/remarkable $OUT/waveforms" "$IMG" || true
  debugfs -R "dump /boot/fitImage.ahab $OUT/boot/vendor-fitImage.ahab" "$IMG" || true
  extract_boot
  echo ">> debugfs extraction done -> $OUT"
  exit 0
fi

echo ">> copying /lib/firmware …"
cp -a "$MNT/lib/firmware/." "$OUT/firmware/" 2>/dev/null || true
echo ">> copying e-ink waveform data from /usr/share/remarkable …"
for f in "$MNT"/usr/share/remarkable/*.eink \
         "$MNT"/usr/share/remarkable/colortable_*.bin \
         "$MNT"/usr/share/remarkable/ct33_*.bin; do
  [[ -e "$f" ]] && cp -a "$f" "$OUT/waveforms/"
done
echo ">> copying /boot/fitImage.ahab …"
cp -a "$MNT/boot/fitImage.ahab" "$OUT/boot/vendor-fitImage.ahab" 2>/dev/null || true
extract_boot

echo
echo ">> done. Extracted into $OUT/"
echo "   firmware:  $(find "$OUT/firmware" -type f | wc -l) files"
echo "   waveforms: $(find "$OUT/waveforms" -type f | wc -l) files"
echo "   boot:      $(find "$OUT/boot" -type f | wc -l) files"
echo ">> copy onto your device per docs/obtaining-vendor-blobs.md"
