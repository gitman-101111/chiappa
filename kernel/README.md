# Kernel — reMarkable Paper Pro Move (chiappa)

The device runs a vendor fork of Linux **6.12.49** (NXP i.MX BSP base).

| | |
|-|-|
| Source | `github.com/reMarkable/linux-imx-rm`, branch `rmpp_6.12.49_v3.26.x` |
| Release tarball | `linux-imx-rel-5.6-vc-3.26.0.68-122eda1b63d9.tar.gz` (git-lfs) |
| Config | [`config-remarkable-chiappa.aarch64`](config-remarkable-chiappa.aarch64) |
| Out-of-tree drivers | all included in the vendor tarball under `3rdparty/` (IW61x WiFi, LIS2DW12, reMarkable platform modules) |

## The config

`config-remarkable-chiappa.aarch64` is a **complete** kernel config: the
vendor's `chiappa_defconfig` plus a handful of deltas (VKMS enabled,
`CONFIG_USB_ETH=n` so a configfs USB gadget can own the UDC, etc.).

⚠️ **Do not reduce it to a fragment.** Feeding a fragment to
`olddefconfig` standalone silently drops `CONFIG_ARCH_MXC` — and with it
the entire i.MX platform (clocks, pinctrl, eMMC, power domains). The
resulting kernel is a generic arm64 build that dies instantly with no
output. A correctly built kernel produces ~156 modules, not ~75.

## Patches

| Patch | Why |
|-------|-----|
| `0001-…-select-VIDEOMODE_HELPERS` | vendor Kconfig for `DRM_PANEL_REMARKABLE_CUMULUS` is missing a `select`; the panel driver fails to link without it |
| `0002-drm-vkms-set-default-mode-954x1696` | VKMS defaults to 1024×768; the panel is 954×1696 portrait. Makes compositors on the virtual display see the native resolution |
| `0003-drm-vkms-allow-cloning-…-encoders` | VKMS never sets `possible_clones`, so an atomic modeset with both the virtual output *and* the writeback connector on the CRTC fails the valid-clone check with EINVAL. Required for the writeback-based display bridge (compositor → VKMS → e-ink) |

Apply order: 0001 → 0002 → 0003 (0003 is generated against a
0002-patched tree).

## Known driver quirks

- **elants_spi (touch)**: runtime-PM autosuspend disables the touch IRQ
  and idles the controller; the resume path does not reliably restore
  scanning, after which no touch/pen events are delivered until the
  controller is re-poked. Workaround: pin runtime PM on via udev —
  see [`../eink/60-chiappa-touch.rules`](../eink/60-chiappa-touch.rules).

## Boot image

The kernel boots as a FIT (`fitImage.ahab`) loaded by U-Boot from the
active rootfs slot — format and build procedure in
[`../docs/fitimage.md`](../docs/fitimage.md). Kernel load/entry address
`0x80400000`; DTBs for hardware revisions F–K are byte-identical in
current firmware (see [`../dtb/`](../dtb/)).
