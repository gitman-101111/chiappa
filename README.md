# chiappa — reMarkable Paper Pro Move

Hardware documentation, firmware, and distro-agnostic tooling for the
reMarkable Paper Pro Move (codename **chiappa**, NXP i.MX 93).

Known integrations built on this layer:
[remarkable-nixos](https://github.com/gitman-101111/remarkable-nixos) (NixOS).

## Device

| | |
|-|-|
| **Product** | reMarkable Paper Pro Move |
| **Codename** | chiappa (ferrari = Paper Pro without Move) |
| **FCC ID** | 2AMK2-RM03AA |
| **SoC** | NXP i.MX 93, 2× Cortex-A55 @ 1.7 GHz + Cortex-M33 |
| **RAM** | 2 GB LPDDR4 (Micron) |
| **Storage** | 64 GB eMMC (Kingston EMMC64G-TY29), GPT |
| **Display** | E Ink Gallery 3 (color), 1696×954, 264 DPI, LVDS via LCDIFv3 |
| **Touch** | Elan SPI (`elants_spi`) |
| **WiFi / BT / NFC** | NXP IW611 combo (WiFi 6 / BT / NFC) |
| **USB** | USB-C OTG, `ci_hdrc_imx` (chipidea), CDC ECM gadget |
| **PMIC** | NXP PCA9451A |
| **Charger** | MAX77818 |
| **Frontlight** | AW99703 (I2C, PWM) |
| **Accelerometer** | ST LIS2DW12 (I2C) |
| **Kernel** | NXP BSP fork of Linux 6.12.49 (`linux-imx-rm`, branch `rmpp_6.12.49_v3.26.x`) |
| **Bootloader** | U-Boot 2025.04 (NXP ELE / AHAB) |
| **HW revisions** | DTBs for rev F–K; all six are byte-identical in current firmware |
| **Vendor OS** | Codex Linux 5.6 / image 3.27.1.0 (Yocto scarthgap) |

## eMMC partition layout

```
mmcblk0        64 GB   whole device (GPT)
mmcblk0p1     100 MB   vendor data / config (persistent across OS flashes)
mmcblk0p2     4.0 GB   ext4   root_a  ← A/B slot
mmcblk0p3     4.0 GB   ext4   root_b  ← A/B slot
mmcblk0p4     1.6 GB           swap (dm-crypt, key from lpgpr bootkey)
mmcblk0p5      48 GB           data / home
mmcblk0boot0   4 MB            bootloader (imx-boot)
mmcblk0boot1   4 MB            bootloader (imx-boot, redundant copy)
```

## Boot chain

```
ELE (S400) → SPL → ATF (BL31) → U-Boot 2025.04 → rM_bootcmd
```

`rM_bootcmd` = `run loadimage; run authimage; run mmcboot`:

| Step | Action |
|------|--------|
| `loadimage` | Load `/boot/fitImage.ahab` from active slot → RAM at `0x8FFFE000` |
| `authimage` | If `unlocked=yes`: skip AHAB auth. Otherwise: enforce `auth_cntr` |
| `mmcboot` | `run mmcverity; run mmcswap; run mmcargs; bootm 0x90000000#${fit_config}` |

`imx-boot` lives in the eMMC boot partitions (`mmcblk0boot0`/`1`). The FIT image
(`fitImage.ahab`, kernel + 6 rev DTBs, no ramdisk) lives in the active rootfs at
`/boot/`. U-Boot selects the DTB config matching the detected PCBA revision.

## Security model

The U-Boot `unlocked` env var — the device's **AHAB lifecycle state** (a one-way
fuse: OEM Open → Closed → Locked) — gates both kernel authentication and the root
device:

| `unlocked` | Kernel auth | Root device |
|------------|-------------|-------------|
| `yes` (OEM Open) | AHAB auth **skipped** — unsigned FITs boot | plain partition (`/dev/mmcblk0pN`) |
| `no` (default) | `auth_cntr` enforced — only reMarkable-signed FITs | dm-verity (`/dev/dm-0`, read-only) |

Enabling **[Developer Mode](https://developer.remarkable.com/documentation/developer-mode)**
(vendor OS settings) moves the device to **OEM Open / `unlocked=yes`** — the
prerequisite for booting a custom kernel — and reveals the SSH/root password
(Settings → Help → About). It wipes user data, so do it first. A vendor recovery
flash does not re-lock the device. Full walkthrough:
[docs/booting-a-custom-os.md](docs/booting-a-custom-os.md).

## Recovery / unbrick

reMarkable ships `rm_recover` (bundled `uuu`) which drives the NXP Serial Download
Protocol (USB SDP) — the device appears as `2edd:0140 "OC Blank 93"` — then loads
`imx-boot`, enters U-Boot fastboot, and flashes eMMC. This path is independent of
the A/B slots and boot counters.

- `rm_recover` is a dynamically linked x86-64 ELF (glibc).
- Recovery server: `https://device-recovery.cloud.remarkable.com/{device,host}/release-latest`
  (current image `3.27.1.0`, machine `chiappa`). Object URLs are runtime-computed
  by the tool; use `rm_recover discover-device` to confirm it sees the device.
- Enter recovery mode: connect USB + hold the power button.

## Subsystem status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| Boot + USB ECM networking | ✅ | configfs gadget, device at 10.11.99.1/24 |
| WiFi (IW611) | ✅ | `sd_w61x_v1.bin.se` required |
| Bluetooth | ✅ | `uartspi_n61x_v1.bin.se` required |
| NFC | ✅ | `ctn730.fw` required |
| Touch (Elan) | ✅ | `elants_spi.bin`; needs the runtime-PM udev fix ([kernel/README](kernel/README.md)) |
| Pen | ✅ | `marker-asic.bin` + `marker-mcu.bin`; same touch controller |
| Frontlight | ✅ | `/sys/.../rm_frontlight` sysfs |
| Accelerometer | ✅ | LIS2DW12 IIO driver |
| Battery / charging | ✅ | MAX77818; mind the power budget ([docs/power.md](docs/power.md)) |
| E-ink display (Gallery 3, color) | ✅ | Via einkbridge — see [E-ink display](#e-ink-display) |
| Wayland on e-ink (VKMS) | ✅ | `vkms_bridge` + `chiappa-compositor`; requires `CONFIG_DRM_VKMS=m` |
| Suspend / wake | ✅ | Power button wake |
| A/B boot slots | ✅ | See [A/B boot slots](#ab-boot-slots) |

## Repository contents

```
chiappa/
├── docs/
│   ├── hardware.md            # component/driver map, partition + boot chain
│   ├── booting-a-custom-os.md # FIT + A/B slots, no-UART deploy, gotchas
│   ├── fitimage.md            # FIT / AHAB container layout, rebuild procedure
│   ├── obtaining-vendor-blobs.md # extracting firmware + Qt/eink runtime
│   ├── eink.md                # e-ink pipeline + low-level frame format
│   ├── power.md               # charging budget, idle draw, low-battery pitfalls
│   └── recovery.md            # SDP unbrick + non-destructive slot switch
├── dtb/                # vendor device trees, rev F–K (firmware/extract-vendor-data.sh)
├── kernel/             # Kernel config + patches + README (build notes)
├── eink/               # e-ink toolkit — see eink/README.md
│   ├── src/            # einkbridge.cpp, vkms_bridge.c, libfakekms.c, swtfb_fill.c, RE tools
│   ├── scripts/        # build-bridge.sh, build-bundle.sh, chiappa-compositor, RE helpers
│   ├── bridge.qml      # display surface loaded by einkbridge
│   ├── 60-chiappa-touch.rules  # touch-controller runtime-PM udev fix
│   └── bin/            # prebuilt aarch64 binaries
├── recovery/           # rm_recover wrapper, uuu scripts, slot-switch.uuu
└── firmware/
    ├── MANIFEST.md     # sha256 BOM for every extracted file
    ├── extract.sh      # Extracts firmware blobs from any production image
    ├── lib/firmware/   # Kernel firmware blobs (WiFi, BT, NFC, pen, touch, TEE)
    ├── waveforms/      # E-ink waveform + colortable files
    └── images/         # Vendor production image archive
```

> Vendor-extracted files (firmware blobs, waveforms, the Qt/eink runtime,
> `rm_recover`, the production image, and the vendor device trees) are
> **not** committed — the paths above are populated by extracting them from
> your own device / firmware image (see
> [docs/obtaining-vendor-blobs.md](docs/obtaining-vendor-blobs.md) and
> [docs/fitimage.md](docs/fitimage.md) for the DTB + AHAB header).

## Quick start for distro porters

### 1. Kernel

Source: `https://github.com/reMarkable/linux-imx-rm`, branch `rmpp_6.12.49_v3.26.x`.
The tarball (`linux-imx-rel-5.6-vc-3.26.0.68-122eda1b63d9.tar.gz`) is distributed
via Git LFS from that repository.

Use `kernel/config-remarkable-chiappa.aarch64` as your `.config`. This is a
**complete config** based on the vendor `chiappa_defconfig` with additional deltas.
It must be used as-is — do not reduce it to a fragment. The kernel build system's
`olddefconfig` applied to a fragment silently drops `CONFIG_ARCH_MXC`, removing
all i.MX platform support (clocks, pinctrl, power domains, eMMC, everything).

Apply the patches in `kernel/` in order (0003 is generated against a
0002-patched tree):
```
0001-drm-panel-remarkable-cumulus-select-VIDEOMODE_HELPERS.patch
0002-drm-vkms-set-default-mode-to-954x1696-for-chiappa-panel.patch
0003-drm-vkms-allow-cloning-virtual-and-writeback-encoder.patch
```
(0002/0003 are only needed for the Wayland/VKMS path.) See
[kernel/README.md](kernel/README.md) for details and driver quirks.

Notable config flags:
- `CONFIG_BLK_DEV_INITRD` **not set** — kernel boots directly to rootfs, no initramfs
- `CONFIG_DRM_VKMS=m` — virtual DRM device, required for Wayland compositors
- `CONFIG_USB_CONFIGFS_F_ECM=y` — USB ECM gadget

### 2. Firmware and vendor blobs

None of the proprietary files (firmware, waveforms, the Qt/eink runtime) are
committed — you extract them from your own device or firmware image. Full
walkthrough: [docs/obtaining-vendor-blobs.md](docs/obtaining-vendor-blobs.md).

Quickest path, straight from the vendor production image:
```bash
cd firmware/
./extract.sh images/remarkable-production-image-3.27.1.0-chiappa-public.ext4.verity.gz
```
(or `extract-vendor-data.sh <vendor-rootfs.img>` if you already have a mounted
or decompressed rootfs.)

Then install `firmware/lib/firmware/` → `/lib/firmware/` and
`firmware/waveforms/` → `/usr/share/remarkable/` on the target.

### 3. Device tree

Use the **vendor-extracted** `chiappa-rev-h.dtb` (produced into `dtb/` by
`firmware/extract-vendor-data.sh`), or whichever revision matches your unit
(check `/proc/device-tree/model` on the vendor OS). All six rev F–K DTBs are
byte-identical in the current firmware. Do NOT use the kernel-tree-built DTB —
it hangs the device early in boot (see [docs/fitimage.md](docs/fitimage.md)).

### 4. FIT image

See [docs/fitimage.md](docs/fitimage.md) for the AHAB container layout and how to
build a `fitImage.ahab` for an unlocked device.

### 5. E-ink display

See [E-ink display](#e-ink-display) below and [docs/eink.md](docs/eink.md).

---

## A/B boot slots

Two symmetric rootfs slots (A = `mmcblk0p2`, B = `mmcblk0p3`). The active slot
lives in the `lpgpr` SNVS registers and is managed with the vendor's `rootdev`
tool (`--active` / `--inactive` / `--next-boot` / `--switch`) — **not** the eMMC
boot partition (`mmc bootpart` only picks which `imx-boot` runs). U-Boot keeps a
per-slot error counter that auto-rolls-back after 3 unreset boots — your safety
net while experimenting.

Rules of thumb: **deploy to the inactive slot, never touch the active one**;
reset the counter after writing a slot; and reset it *late* in boot (after SSH is
up) so it's a genuine "boot succeeded" signal. Full mechanism, deploy recipe, and
the SDP non-destructive slot switch:
[docs/booting-a-custom-os.md](docs/booting-a-custom-os.md),
[docs/recovery.md](docs/recovery.md).

---

## E-ink display

The pipeline is **entirely in userspace** — raw DRM scanout to `/dev/dri/card0`
shows nothing; the cumulus panel needs the vendor waveform engine
(`libqsgepaper` / SWTCON) driving temporally-encoded frame sequences.
`einkbridge` (a Qt Quick app on the `epaper` QPA) drives it and exposes an
**rm2fb-compatible IPC** (`/dev/shm/swtfb` + `/tmp/swtfb.ipc`) so KOReader and
other rm2fb-aware apps work unmodified. `vkms_bridge` extends this to Wayland
compositors via the VKMS writeback connector (needs `CONFIG_DRM_VKMS=m` + kernel
patches 0002/0003).

Full pipeline, frame format, IPC structs, build/bundle steps, and the SWTCON
constraints: [docs/eink.md](docs/eink.md) and [eink/README.md](eink/README.md).

## License

MIT, **except** [`kernel/`](kernel/) (patches + config derived from the Linux
kernel — GPL-2.0-only, see [`kernel/LICENSE`](kernel/LICENSE)).
Vendor-extracted artifacts (firmware, waveforms, the Qt/eink runtime, and the
vendor device trees — whose corresponding source is not published) are never
part of this repo; you extract them from your own device.
