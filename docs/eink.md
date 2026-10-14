# E-ink display pipeline — chiappa

How the reMarkable Paper Pro Move drives its **E Ink Gallery 3 (color)** panel
and how to drive it from a custom Linux installation.

## Summary

The panel is **E Ink Gallery 3** — a color e-paper display (1696×954, 264 DPI).
The display pipeline is **standard DRM/KMS with a reMarkable-specific frame
format**. The kernel drivers (`imxdrm`, `imx93-parallel-disp-fmt`,
`imx-lcdifv3-crtc`, `panel-rm-cumulus`) are built-in and work correctly. The
display requires a **userspace driver** — the kernel DRM side presents
`/dev/dri/card0` but raw scanout produces no visible output without the correct
temporal frame sequences that encode the e-ink waveform.

The working approach is to reuse the vendor's `libqsgepaper` waveform engine via
a Qt Quick application and the `epaper` QPA plugin. This is what `einkbridge`
does. `libqsgepaper` handles both B&W (DU fast path) and full Gallery 3 color
rendering automatically based on the content being displayed. See
`../eink/src/einkbridge.cpp`.

## Hardware pipeline

```
LCDIFv3 → parallel display format converter (fsl,imx93-parallel-display-format, RGB565) → remarkable,cumulus-panel
```

- DRM device: `/dev/dri/card0`, connector `LVDS-1` (id 36; the panel driver registers an LVDS connector type, but the wire is the SoC's 16-bit parallel RGB bus — pads `MEDIAMIX_DISP_DATA00..15` in the DT), CRTC id 34, plane id 32
- Transport mode: **365×1700 @ 85 Hz**, pixel clock 56 MHz
- Pixel format: **RGB565** (`RG16`), 2 bytes/px, stride 730
- Frame size: `365 × 1700 × 2` = 1,241,000 bytes

The `cumulus` controller converts the scanned-out frames to e-ink drive signals.
Waveform selection is not out-of-band — **the frame sequence is the waveform**.

> **Cumulus disambiguation.** This panel is driven **purely over the parallel display bus** — no LVDS, no MIPI-DSI,
> no FPGA bitstream, no EPDC/PXP. The BSP also ships `rm-cumulus-bridge.c` (an
> i2c + fpga-mgr + DSI driver), but that is for the **bigger Paper Pro (`ferrari`)**,
> NOT chiappa — ignore it when reading vendor source.

## Frame format

Each 365×1700 RGB565 frame requires a fixed control structure. Frames missing
this structure are silently ignored by the controller.

| Region | Content |
|--------|---------|
| Row 0 | Header: `0x2000` in cols 1–22, `0x3000` mid; constant |
| Col 0 | `0x0000` |
| Cols 1–18 | `0x8000` — vertical sync stripe (constant) |
| Cols 19–22 | `0x0000` gap |
| Cols 23–57 | `0x1000` + per-row `0x5000` marker (fixed structure) |
| **Cols 58–274, rows 208–1578** | **Active image region** |
| Cols 275–364 | `0x0000` / `0x1000` (fixed) |

Only the active image region (cols 58–274 × rows 208–1578) should be modified.
All other bytes must be left byte-identical to a valid vendor frame. The active
region maps approximately linearly to the visible screen area.

### Pixel values (DU / fast B&W waveform)

These values appear in the active region when libqsgepaper is operating in the
Direct Update (DU) waveform mode — a 2-level (black/white only) fast-path.

| Value | Meaning |
|-------|---------|
| `0x0000` | No-op |
| `0x1000` | Hold (no-op in partial mode; white under a full-clear sequence) |
| `0x1db6` | Black (darkest content value) |
| `0x16db`, `0x1492` | Full-screen flash / clear levels |
| `0x8000` | Sync stripe value |

`colortable_*.bin` is a 65536×8 LUT indexed by the 16-bit transport pixel value,
corroborating these B&W mappings. In DU mode, any gray value (between `0x0000`
and `0x1db6`) is thresholded to one of the two levels and does not render as gray.

### Color rendering (Gallery 3)

The panel is **E Ink Gallery 3** (color e-paper). Full-color and grayscale
rendering requires a multi-frame waveform sequence rather than the DU fast path.
libqsgepaper selects the waveform automatically based on the rendering mode
requested via `EPScreenModeItem`.

Color waveform tables live alongside the B&W tables in `/usr/share/remarkable/`:

| File pattern | Purpose |
|---|---|
| `GAL3_<lot>_*.eink` | Per-lot color waveform (libqsgepaper selects by panel serial) |
| `ct33_fast.bin` | Gallery 3 color LUT — fast update (lower quality) |
| `ct33_std.bin` | Gallery 3 color LUT — standard quality |
| `ct33_best.bin` | Gallery 3 color LUT — best quality (slowest) |
| `ct33_pen.bin` | Gallery 3 color LUT — optimized for pen/stylus input |
| `colortable_*.bin` | B&W/grayscale LUT (same quality tiers) |

The `ct33_*.bin` files map RGB input values to EPD drive sequences for the
Gallery 3 color particle set. Color rendering uses significantly more frames
than DU and takes noticeably longer to settle on screen.

## Update mechanism

The vendor application (`xochitl`) drives the display via
`DRM_IOCTL_MODE_ATOMIC` at 85 Hz, page-flipping through a ring of framebuffers.
Only the `FB_ID` plane property changes per commit — there is no out-of-band
waveform or EPD property.

Full refresh vs. partial update differs only in the frame sequence content:

- **Full refresh**: the sequence includes black flash frame(s) before the content
  frame (`[black ×N] → [white ×N] → [content]`), de-ghosting the panel
- **Partial update**: content frame only, may leave ghosting

Each framebuffer in the sequence is held for approximately 140 ms (~12 atomic
commits) before advancing. A single held frame will drive the panel slowly toward
its target values over approximately 6 seconds (~510 scans).

## Using the vendor waveform engine

`libqsgepaper.so` provides `EPFramebuffer` / `EPFramebufferSwtcon`, a software
TCON with a phase-generator thread. It loads the panel's `.eink` waveform file,
sets the EPD bias voltage, reads panel temperature, and drives `/dev/dri/card0`.

The correct entry point is the **Qt Quick + `epaper` QPA** path, not the
lower-level `setBuffers`/`swapBuffers` API. The QPA plugin owns the buffer
protocol and handles timing internally.

Launch:
```sh
QT_QUICK_BACKEND=epaper  QT_QPA_PLATFORM=epaper \
QT_PLUGIN_PATH=/usr/lib/plugins  QT_QPA_FONTDIR=/usr/share/fonts \
./einkbridge  scene.qml
```

The `epaper` platform reports `screenGeometry 954×1696` and wires evdev touch
and keyboard input. The `EPScreenModeItem` QML type exposes per-region refresh
mode control (`Pen` / `Mono` / `Animation` / `UI` / `Content`).

### Build

Cross-compile for aarch64-glibc:
```sh
aarch64-unknown-linux-gnu-g++ -std=c++20 \
  $(pkg-config --cflags Qt6Quick Qt6Qml Qt6Gui Qt6Core) \
  app.cpp -L <vendor-Qt6-lib-dir> \
  -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Core \
  -Wl,--unresolved-symbols=ignore-in-shared-libs
patchelf --set-interpreter /lib/ld-linux-aarch64.so.1 app
```

Use a consistent Qt6 version for headers and libraries. See
`../eink/scripts/build-bridge.sh`.

### Runtime constraints

- **Single instance**: SWTCON takes an exclusive lock at `/tmp/epframebuffer.lock`.
  Two concurrent instances will conflict.
- **`/tmp/epd.lock`** must exist (zero-byte, execute bit set) before launch.
- **`/dev/dri/card0`** must not be held open by any other process.
- On musl-based distros, the entire Qt6 + libqsgepaper runtime must be bundled
  as a self-contained glibc environment (conventionally at `/opt/eink/`).

## Display bridge architecture

All userspace display access must go through the vendor engine. The display
bridge (`einkbridge`) provides:

1. **Direct rendering** via Qt Quick QML scenes (einkbridge + bridge.qml)
2. **rm2fb IPC** — shared memory at `/dev/shm/swtfb` (954×1696 RGB565) and
   Unix socket at `/tmp/swtfb.ipc` for rm2fb-compatible applications (KOReader etc.)
3. **VKMS bridge** (`vkms_bridge`) — captures a Wayland compositor's output
   from the VKMS virtual DRM device (`/dev/dri/card1`) via its writeback
   connector (the compositor drives a DRM-leased plane) and forwards frames to
   the rm2fb layer. See the chiappa README's "VKMS bridge" section

## Alternative: reimplementing from the `.eink` format

The `.eink` format is reMarkable's custom waveform format (not standard `.wbf`).
`colortable_*.bin` is a 65536×8 per-value drive-curve LUT. A fully open
implementation would decode these files and implement the phase-generator
directly, using `libqsgepaper` as an oracle during development.

This panel's waveform lot: `AAB0B7` (from `devconfig serial_number_epd`),
VCOM −0.58 V. Waveform files are in `../firmware/waveforms/`.

### Capturing vendor frames (reverse-engineering method)

To decode what the vendor engine actually does, you must capture its frames — but
xochitl's DRM framebuffers **cannot be read via `/proc/<pid>/mem`** (they're
`VM_PFNMAP` → `EIO`). The working method is an **`LD_PRELOAD` shim inside xochitl**
that hooks `drmModeSetCrtc` / `ioctl` and traces/decodes the `MODE_ATOMIC` commits
(the `epshim` variants) — this is how "the frame sequence *is* the waveform, no
out-of-band trigger" was proven. Any fully-open engine reimplementation would redo
this to validate against `libqsgepaper`.
