# Obtaining vendor blobs (firmware & e-ink waveforms)

We **cannot redistribute** reMarkable's or NXP's proprietary files. But you own your
device, and reMarkable publishes a recovery image for it — so you can obtain the bits
you need yourself, legally, from that image (or from your running vendor OS). This
page + the `firmware/` and `recovery/` helper scripts walk you through it.

You'll end up with four things (one `firmware/extract-vendor-data.sh` run
produces them all):
- **Wi-Fi/BT/NFC + misc firmware** → your rootfs `/lib/firmware/`
- **e-ink waveform tables** (`*.eink`, `colortable_*.bin`, `ct33_*.bin`) → wherever your
  e-ink driver reads them (the vendor keeps them in `/usr/share/remarkable/`)
- **the 8 KB AHAB header** from the vendor fitImage → prepended to any FIT you
  build ([fitimage.md](fitimage.md))
- **the vendor device trees** (rev F–K, byte-identical) → embedded in your FIT;
  the kernel-tree-built DTB does not boot this device ([fitimage.md](fitimage.md))

> None of these files are in this repo. The steps below produce them on **your** machine.

## Option A — from the public recovery image (recommended)

reMarkable distributes recovery via the `rm_recover` tool, which pulls a per-device
image from a public bucket (`device-recovery.cloud.remarkable.com`, machine `chiappa`).

1. Get reMarkable's `rm_recover` tool:
   [Linux](https://developer.remarkable.com/documentation/recovery-for-linux-host)
   · [macOS](https://support.remarkable.com/s/article/Software-recovery).
   (On Windows the recovery flow goes through the desktop app; the Linux/macOS
   tool is the known-good path for extracting the image.)
2. Download the recovery artifacts (don't necessarily flash):
   ```sh
   ../recovery/fetch-recovery-image.sh      # see the script for details/URLs
   ```
   This yields a vendor rootfs image (an ext4 + dm-verity blob) and `imx-boot`.
3. Decompress + mount the rootfs read-only and extract what you need:
   ```sh
   ../firmware/extract-vendor-data.sh /path/to/vendor-rootfs.img ./out
   ```
   → `out/firmware/` (the `/lib/firmware` tree) and `out/waveforms/` (`/usr/share/remarkable`).

## Option B — from your running vendor OS

If you still have the stock OS on the other A/B slot, copy directly off it:
```sh
# over USB/SSH to the vendor OS (its rootfs is read-only):
scp -r root@10.11.99.1:/lib/firmware ./out/firmware
scp -r root@10.11.99.1:/usr/share/remarkable ./out/waveforms
```
(Or mount the inactive vendor slot from your custom OS and copy from there.)

## What to copy where

```sh
# firmware (Wi-Fi/BT/NFC, touch, NFC controller, regulatory.db, etc.)
cp -a out/firmware/*  <your-rootfs>/lib/firmware/

# e-ink waveform data (used by the e-ink driver)
cp -a out/waveforms/{*.eink,colortable_*.bin,ct33_*.bin} <your-rootfs>/usr/share/remarkable/
```

Pick the **`.eink` file matching your panel's lot**. The lot code is in the EPD serial
(`devconfig`-style `serial_number_epd`, e.g. `…AAB0B7…` → `GAL3_AAB0B7_*.eink`). See
[eink.md](eink.md).

## Also obtain: Qt6 runtime + the e-ink Qt plugins (for the reuse-based driver)

The working e-ink driver ([eink.md](eink.md)) reuses the vendor's waveform engine, which
is a set of glibc Qt6 libraries + two Qt plugins. Extract these from your device / the
recovery rootfs too:

```sh
# Qt6 runtime (from the vendor rootfs /usr/lib)
cp -a  <mnt>/usr/lib/libQt6*.so.*      <bundle>/lib/
# the e-ink QPA platform + scenegraph backend plugins
cp -a  <mnt>/usr/lib/plugins/platforms/libepaper.so     <bundle>/plugins/platforms/
cp -a  <mnt>/usr/lib/plugins/scenegraph/libqsgepaper.so <bundle>/plugins/scenegraph/
cp -a  <mnt>/usr/lib/plugins/imageformats/*.so          <bundle>/plugins/imageformats/
# their non-Qt deps (ICU, freetype, fontconfig, harfbuzz, png, glib, …) + libstdc++,
# and — on a musl distro — the glibc loader + libc, to run glibc binaries via a bundle.
```

On a **glibc** distro you mostly just need Qt6 + the two plugins. On
**musl** (e.g. Alpine) you bundle the whole glibc+Qt6 closure and run via the vendor
`ld-linux-aarch64.so.1`. These are reMarkable/Qt proprietary/LGPL binaries — obtain from
your own device; do not redistribute.

`eink/scripts/build-bundle.sh` (with `eink/scripts/closure.py`) automates this
whole closure into `eink/bundle/eink/` — point it at a mounted vendor rootfs
(`VROOT`) and it assembles the loader + Qt6 + plugins + your panel's waveform
into a self-contained `/opt/eink/` bundle.

## Notes / legality

- This is interoperability work on hardware you own; you're extracting firmware that
  shipped on your device for use on that same device.
- Do not commit any extracted files to a public repo. `.gitignore` here already blocks
  the common names, but double-check before pushing.
- NXP Wi-Fi/BT firmware also ships in `linux-firmware` and NXP's repos in some versions;
  if a compatible build is available there under a redistributable license, prefer it.
