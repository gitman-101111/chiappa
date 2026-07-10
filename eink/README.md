# e-ink display toolkit (chiappa)

Everything needed to drive the reMarkable Paper Pro Move's **E Ink Gallery 3**
panel from a custom Linux install. Background on how the panel works:
[`../docs/eink.md`](../docs/eink.md).

The panel has no conventional Linux driver — the kernel presents `/dev/dri/card0`
but raw scanout shows nothing, because the controller needs temporal frame
sequences that encode the e-ink waveform. The working approach reuses the
vendor's `libqsgepaper` waveform engine via its `epaper` Qt platform plugin,
wrapped in a small bridge that exposes a simple shared-memory + socket protocol
(rm2fb-compatible, so KOReader and similar apps work unmodified).

## Layout

| path | what |
|---|---|
| `src/einkbridge.cpp` | the display bridge: `/dev/shm/swtfb` + `/tmp/swtfb.ipc` → `epaper` QPA → SWTCON → panel. Event-driven (repaints only on client updates). |
| `src/vkms_bridge.c` | VKMS writeback → rm2fb bridge: puts a Wayland compositor on the panel (leases the VKMS plane, captures via writeback, dirty-rect + waveform pick). Needs kernel patches 0002+0003. |
| `src/libfakekms.c` | `LD_PRELOAD` shim handing a wlroots compositor the `vkms_bridge` DRM lease. |
| `src/swtfb_fill.c` | fill the framebuffer a solid value + full refresh (panel clear / smoke test). |
| `src/bridge_test.c` | example rm2fb client (draws a gradient + checkerboard). |
| `bridge.qml` | display surface loaded by einkbridge (event-driven; boot banner until first content). |
| `60-chiappa-touch.rules` | udev rule pinning the touch controller's runtime PM on (see [`../kernel/README.md`](../kernel/README.md)). |
| `scripts/build-bridge.sh` | cross-compile `einkbridge` (aarch64, glibc). |
| `scripts/build-bundle.sh` + `closure.py` + `fill-closure.sh` | assemble the self-contained glibc/Qt6 runtime bundle → `/opt/eink/`. |
| `scripts/chiappa-eink-bridge` | launcher: sets up the bundle env + locks and execs einkbridge. |
| `scripts/chiappa-compositor` | launcher: injects the VKMS lease via `libfakekms.so` and execs a Wayland compositor. |

Waveform data (`GAL3_*.eink`, `colortable_*.bin`, `ct33_*.bin`) lives in
[`../firmware/waveforms/`](../firmware/waveforms/) — extract it from your own
firmware image (see [`../docs/obtaining-vendor-blobs.md`](../docs/obtaining-vendor-blobs.md)).
`bundle/eink/` (the runtime bundle for musl-based distros) and `bin/` (prebuilt
binaries) are generated locally and not committed.

## The rm2fb protocol

Any application can drive the display by writing to shared memory and poking a
socket — no vendor libraries needed on the client side:

- **`/dev/shm/swtfb`** — 954×1696×2 bytes, RGB565, world-writable.
- **`/tmp/swtfb.ipc`** — clients send `struct swtfb_update { rect{top,left,width,height}; waveform; flags; }`.

`einkbridge` copies the dirty rectangle into its QML surface and renders it
through the vendor engine, which picks the waveform (color content uses the
Gallery 3 color tables; B&W uses the faster DU path).

## Building

**einkbridge** — cross-compile for aarch64-glibc; it links the vendor Qt 6.8.2
`.so` stubs from the bundle and runs against `/opt/eink/lib` on the device:
```sh
CXX=aarch64-linux-gnu-g++ \
QT_BASE=<qt6-prefix> QT_QML_HDRS=<qt6-declarative-include> \
scripts/build-bridge.sh
```
Keep to Qt ≤6.8 APIs (see the script header for why).

**Runtime bundle** — `scripts/build-bundle.sh` stages the vendor glibc loader +
Qt6 + `libqsgepaper` + plugins into `bundle/eink/` (deploy to `/opt/eink/` on a
musl distro). Inputs are documented at the top of the script (`VROOT` = a
mounted vendor rootfs, `PANEL_LOT` = your panel's EPD lot).

**Device tools** (`swtfb_fill`, `bridge_test`) — plain aarch64 C, no Qt:
```sh
aarch64-linux-gnu-gcc -O2 -o swtfb-fill src/swtfb_fill.c
```

## On-device runtime notes

- SWTCON needs **exclusive** access to `/dev/dri/card0` — nothing else may hold
  it open while einkbridge runs.
- `/tmp/epd.lock` must exist (zero-byte) before einkbridge starts; the
  `chiappa-eink-bridge` launcher handles this.
- On musl distros the whole Qt6 + libqsgepaper runtime must come from the
  self-contained glibc bundle at `/opt/eink/`.
