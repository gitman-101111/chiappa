#!/usr/bin/env bash
# Fetch reMarkable's public recovery artifacts for chiappa (Paper Pro Move).
#
# reMarkable's `rm_recover` tool downloads a per-device image from a public bucket
# (device-recovery.cloud.remarkable.com, machine=chiappa). The exact object URLs are
# computed by the tool at runtime, so the supported path is to run rm_recover itself.
# This wrapper just documents/automates invoking it and pinning the output dir.
#
# You must provide rm_recover (Linux build) from reMarkable's support site.
# We do NOT redistribute it.
set -euo pipefail

RM_RECOVER="${RM_RECOVER:-./rm_recover}"     # path to reMarkable's tool
OUT="${1:-./recovery-out}"

if [[ ! -x "$RM_RECOVER" ]]; then
  cat <<EOF
rm_recover not found at: $RM_RECOVER

1. Download reMarkable's Linux 'rm_recover' from their support site.
2. Re-run:  RM_RECOVER=/path/to/rm_recover $0 [outdir]

rm_recover is a dynamically linked x86-64 ELF (glibc).
It honors TMPDIR=<dir> to pin where it stores downloaded artifacts.
EOF
  exit 1
fi

mkdir -p "$OUT"
echo ">> Downloading recovery artifacts into $OUT (TMPDIR pin)…"
# 'restore' downloads then flashes; if you only want the files, you can interrupt
# after download, or use a download-only subcommand if your rm_recover version has one.
TMPDIR="$OUT" "$RM_RECOVER" restore || true

echo
echo ">> Look in $OUT for:"
echo "   *-chiappa-public.ext4.verity.gz   (vendor rootfs)"
echo "   *-chiappa-public-imx-boot         (bootloader)"
echo ">> Then extract firmware/waveforms with: scripts/extract-vendor-data.sh <rootfs.img> ./out"
