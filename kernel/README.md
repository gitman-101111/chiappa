# Kernel — reMarkable Paper Pro Move (chiappa)

Upstream Linux for chiappa: a patch series on top of a stable mainline
release, a config fragment, and the complete config generated from it. The
vendor 6.12.49 BSP kernel this replaced lives on the `vendor` branch.

> **Status: runs the full system on hardware** (brought up 2026-10-12/13):
> display through the PDFC bridge, touch/pen, front light, accelerometer,
> WiFi, Bluetooth driver, NFC pen charger, MAX77818 charging and fuel gauge,
> A/B rollback counters, cpufreq, and deep suspend with power-key wake.
> Standby draw is about 13 mA, against an estimated 8-9 mA on the vendor
> kernel (see below). Flash a new build to the inactive A/B slot first; the
> U-Boot three-strike rollback brings the other slot back.

| | |
|-|-|
| Validated on | Linux **7.2.10**. The series is meant to follow the latest stable kernel; "Updating the kernel" below covers a patch that stops applying |
| Series | `0001-…` → `0017-…` in this directory, apply in order with `git am` / `patch -p1`. `0003` backports a driver 7.3 has; drop it there |
| Fragment | [`chiappa-mainline.config`](chiappa-mainline.config), merged over `make defconfig` |
| Config | [`config-chiappa-mainline.aarch64`](config-chiappa-mainline.aarch64) — the complete `.config` to build with |
| DTB | `freescale/imx93-remarkable-chiappa.dtb` (kernel-tree; the vendor DTB describes vendor bindings and does not boot this kernel) |
| WiFi | out of tree — NXP `mwifiex` tag `lf-6.18.37-2.1.0` (`moal.ko` + `mlan.ko`) with the patches in [`mwifiex/`](mwifiex/) applied: the `probe_peer` guard, and the by-value cfg80211 cookies of 7.3 (both version-guarded) |

## What the series carries

Mainline covers the i.MX93 SoC side: clocks, pinctrl, power domains, eMMC,
ChipIdea USB, LPUART/LPI2C/LPSPI, eDMA, BBNSM RTC and power key, OCOTP fuses,
the LCDIF display controller, cpufreq (from the OPP table in the DT),
Bluetooth (`btnxpuart`), the LIS2DW12 accelerometer (`st_accel`), and the
PCA9451A PMIC. The series adds what the device needs beyond that:

| Patch | What | Origin |
|-------|------|--------|
| `nvmem/snvs_lpgpr` (0001) | i.MX93 BBNSM general-purpose registers (`fsl,imx93-bbnsm-gpr`) — the A/B slot state U-Boot reads. Registers the DT child cells (`add_legacy_fixed_of_cells`) and assembles the byte-wise accesses the nvmem core makes on bit-field cells from whole 32-bit registers | vendor driver's imx93 table entry |
| `platform/remarkable/lpgpr` (0002) | `/sys/devices/platform/lpgpr` (root slot, error counters, rescue flags); platform-keyring code dropped | vendor, trimmed |
| `drm/bridge/imx/imx93-pdfc` (0003) | the parallel display format converter driver, backported from 7.3 (one line adapted: 7.2 resets bridge state with `.atomic_reset`): mainline 7.2 has the `nxp,imx93-pdfc` DT node but no driver, and the LCDIF cannot find its bridge without one. **Drop on 7.3 and later** | upstream 7.3 |
| `drm/bridge/imx/imx93-pdfc` (0004) | an optional "gpio" pinctrl state that parks the display pads while the output is disabled (the vendor driver did the same, to avoid leakage through the e-paper panel) | this repo |
| `drm/panel/panel-rm-cumulus` (0005) | the e-paper panel (RGB565 over the 16-bit parallel bus; connector type stays LVDS so userspace still sees `LVDS-1`); `/sys/devices/platform/cumulus-panel/*` rail attributes kept | vendor, ported to `devm_drm_panel_alloc` |
| `drm/vkms` (0006) | 954×1696 portrait default mode (the clone patch from the vendor tree is already upstream) | this repo |
| `regulator/g2194` (0007) | EPD bias PMIC (`/sys/bus/i2c/drivers/g2194-regulator/0-0048/vpdd_length` is read by the vendor waveform engine) | vendor, busfreq calls removed |
| `mfd`/`power/supply`/`regulator` MAX77818 (0008) | charger + fuel gauge (`max77818-charger`, `max77818_battery` power supplies) | vendor |
| `usb/chipidea` (0009) | publishes the gadget on the usb_phy OTG descriptor; `fusb303b` reads its state to tell a configured connection from a bare attach, and otherwise leaves the input current at 100 mA | downstream i.MX tree |
| `usb/typec/fusb303b` (0010) | Type-C port controller; `queue_charging_negotiation` hook | vendor |
| `input/touchscreen/elants_spi` (0011) | Elan touch + pen controller | vendor, busfreq calls removed |
| `video/backlight/aw99703_bl` (0012) | front light (`rm_frontlight`) | vendor |
| `mfd`/`gpio`/`regulator`/`platform` SLG46824 (0013) | GreenPAK wake controller and its virtual GPIOs (NFC power enable lives there) | vendor |
| `nfc/ctn730_rm` (0014) | marker (pen) wireless charger | vendor |
| `hwmon/ntc_thermistor` (0015) | the TDK NTCG103JF103FT1 panel thermistor table; the waveform engine picks its waveform by panel temperature and never initialises without a hwmon `temp1_input` (blank display, 1404x1872 fallback geometry) | vendor |
| board DT (0016) | `imx93-remarkable-chiappa.dts`, rewritten from the vendor `chiappa.dtsi` against mainline bindings (parallel display graph, BBNSM GPR cells, aliases, `ramoops`) | this repo |
| `pmdomain/imx93-blk-ctrl` (0017) | system-sleep callbacks that hold a runtime-PM reference across suspend, as `imx8m-blk-ctrl` does; without them the noirq resume fails to power the LCDIF domain back up and a real deep sleep ends in a SoC reset | after `imx8m-blk-ctrl` / vendor |

Vendor-only infrastructure that has no mainline counterpart and is left out:
`imx93-lpm` (bus/DDR frequency scaling, `busfreq-imx`), the EdgeLock `se`
driver, `bootlog`, `lptmr-btc`, `sleep-monitor`, `suspend-event`,
`pcba-bomrev`, and the `PM_PRE/POST_AUTOSLEEP` notifier events (the drivers
that used them compile them out).

## Known differences from the vendor kernel

- **Display timing.** The vendor LCDIF driver has a `flip-before-wait` mode
  (atomic commits return before the vblank wait); mainline `lcdif` uses the
  standard commit tail. The waveform engine drives the panel at 85 Hz with
  legacy `drmModeSetCrtc`/page flips; no latency difference has been noticed
  in use, but it has not been measured.
- **Bus frequency scaling.** No `busfreq` → the SoC buses stay at their boot
  rates; the vendor touch/EPD drivers' "boost for one second" calls are gone.
  Expect different (likely higher) idle power until an equivalent exists.
- **Suspend.** Mainline suspend goes through PSCI to the vendor TF-A without
  the vendor `lpm`/`sleep-monitor` plumbing. Deep sleep and power-key wake
  work. Measured 2026-10-14 from the fuel gauge's coulomb counter over an
  11.5 h deep sleep (WiFi unloaded, NFC charger unbound): **13 mA**; awake
  and idle on a static page with WiFi associated: about 160 mA. The vendor
  kernel has no measurement on record; the figures recorded while tuning its
  sleep hooks imply roughly 8-9 mA with the same mitigations, so expect mainline standby
  to be somewhat higher until the vendor low-power-mode plumbing has an
  equivalent. A raw `echo mem > /sys/power/state` is
  refused with `-EAGAIN` while the g2194 VPDD hold-off timer runs — suspend
  through systemd so the e-ink sleep screen powers the panel down first.
- **Accelerometer.** The driver is `st-accel-i2c` (vendor: `st_lis2dw12_i2c`)
  and keeps the sensor powered down unless an IIO buffer is enabled, so it is
  not unbound across suspend (rebinding it stormed its IRQ). Its INT1 line
  sits low while powered down, so the DT declares it edge-triggered: as a
  level IRQ it stormed at boot and, being a wakeup source, aborted every
  suspend as a pending wake.
- **WiFi.** Module names are NXP's `moal`/`mlan`, not the vendor tree's
  `iw61x_sdw61x`/`iw61x_mlan`. The driver reads its parameters from the
  `sdxxx-wlan` DT node, including `fw_name` (pinned to
  `nxp/sd_w61x_v1.bin.se`). NXP's SDIO ID table has no entry for the IW611
  (`0471:0205`), so nothing autoloads `moal`; load it explicitly. A soft
  rfkill block restored from `/persist` by `systemd-rfkill` keeps `wlan0`
  down until `nmcli radio wifi on`.
- **KoReader** treats any kernel at or above 6.2 without the vendor `csl`
  tool as the reMarkable 2 mainline port and opens that device's input paths
  (the vendor 6.12 only escaped because `6.12 < 6.2` as a float);
  integrations have to patch that check.
- **Watchdog.** `systemd-shutdown` logs `Failed to set watchdog hardware
  timeout to 10min`: the i.MX93 watchdog tops out at 128 s, so the reboot
  watchdog is not armed. Same as the vendor kernel.
- **Harmless noise:** `ramoops: uncorrectable error in header` on the first
  boot with a fresh reservation, `slg46824-wakeup: Failed to get
  irq-mask-folio gpio` (a deferred-probe attempt before the SPLD GPIO chip is
  up; the retry binds), `st-accel-spi has no spi_device_id`, and
  `max77818-regulator`/`slg46824-regulator` "Failed to locate of_node" (no
  regulator child nodes in the DT, same as the vendor kernel).

## Regenerating the config

```sh
# in a tree with the series applied, with an aarch64 cross gcc on PATH
./regenerate-config.sh /path/to/linux
```

The script refuses to publish a config in which a fragment option was lost
to an unmet dependency. The fragment also disables every non-NXP ARM64
platform of the defconfig so the kernel stays small enough to build under
emulation (an x86_64 host building through binfmt).

## Updating the kernel

The series is meant to follow the latest stable kernel. Point releases take
new config symbols at their defaults (`make oldconfig` with no terminal) and
have applied cleanly so far; a new series is where patches and config need
work. When a patch stops applying, or ahead of a planned series bump:

1. Unpack the new tree, `git init` + commit it, then `git am` this
   directory's `*.patch` onto it. Fix whatever does not apply or compile.
   On 7.3 and later, delete `0003` first: it backports a driver those
   kernels already have, and the fragment already names its upstream symbol.
2. `git format-patch -N --no-signature <pristine>..HEAD` back into this
   directory (delete the old `0*.patch` first).
3. `./regenerate-config.sh <tree>`; it fails loudly if a fragment option was
   lost to a renamed or removed symbol.
4. Cross-build `Image modules dtbs` once natively before an emulated
   build.
5. Flash the inactive slot, run the checklist below, and update the
   "validated on" version above.

## Debugging without a UART

The DT reserves a `ramoops` region (1 MiB at `0xa4000000`) and the config
enables `pstore` with console and panic records, so a crash is readable from
`/sys/fs/pstore` on the next boot of a pstore-capable kernel. `IKCONFIG` is
on (`/proc/config.gz`). A kernel that never answers on USB leaves its log in
pstore: boot the other slot (or this one again) and read `/sys/fs/pstore/`
if that kernel has `CONFIG_PSTORE_RAM` and the same `ramoops` reservation.

Two things that do not work: `ramoops` does not survive a SoC reset (as
opposed to a panic), and netconsole over the USB gadget re-enumerates the
gadget when enabled, dropping the host link and the SSH session that started
the test. For suspend problems the previous boot's journal
(`journalctl -b -1 -k`) is the reliable record; `CONFIG_PM_DEBUG` plus
`pm_test` narrows the failing stage when needed (not in the shipped config).

## Bring-up checklist

Flash to the inactive slot with the vendor U-Boot's three-strike rollback
armed (do not clear the error counter by hand). Over the USB link, check in
this order:

1. `cat /proc/device-tree/model` says "reMarkable Chiappa" (the vendor string, which KoReader keys on) and
   `uname -r` is `7.2.10-chiappa` — the FIT carried the kernel-tree DTB.
2. `/sys/devices/platform/lpgpr/root_part` matches the booted slot and the
   errcnt service cleared `root{a,b}_errcnt` after `multi-user.target`.
3. `dmesg | grep -iE 'lcdif|pdfc|cumulus|g2194'` — the panel must bind
   through the PDFC bridge and `/dev/dri/card0` must list connector
   `LVDS-1`. Only then start the e-ink bridge. hwmon must list
   `ntcg103jf103ft1`, or the waveform engine never initialises.
4. `/sys/class/power_supply/{max77818-charger,max77818_battery}`,
   `/sys/bus/i2c/devices/1-0021/queue_charging_negotiation` (fusb303b),
   `/sys/class/backlight/rm_frontlight`, `/sys/bus/i2c/drivers/st-accel-i2c`,
   `/sys/devices/system/cpu/cpufreq/policy0`.
5. `modprobe moal` brings up `wlan0` (firmware `nxp/sd_w61x_v1.bin.se`);
   `btnxpuart` binds `hci0` after it.
6. A systemd-initiated suspend sleeps deep (`PM: suspend entry (deep)`,
   `Timekeeping suspended for N seconds`) and wakes on the power button with
   the rollback counters untouched; then compare standby draw against the
   vendor kernel before trusting it overnight.

## Bring-up log (2026-10-12/13)

What the hardware taught, in order, and where each lesson now lives:

- The BBNSM GPR cells were not registered (`lpgpr` and `nvmem-reboot-mode`
  failed with `-ENOENT`), and the first cell write then failed with
  `-EINVAL` because the nvmem core accesses bit-field cells byte-wise →
  patch 0001.
- `fusb303b` logged "Unable to obtain gadget state" and left the input
  current at 100 mA → patch 0009.
- The display stayed blank: no driver for the panel thermistor → patch 0015.
- No `cpufreq-dt` device: an "allow i.MX93" patch had been anchored in
  cpufreq-dt-platdev's blocklist rather than its allowlist and prevented the
  very device it meant to create. Dropped; with an OPP table in the DT the
  generic path registers `cpufreq-dt` by itself.
- WiFi did not autoload and stayed rfkill-blocked; KoReader opened the rM2
  input paths → see "Known differences" above.
- Deep suspend reset the SoC a few seconds after `PM: suspend entry (deep)`
  and U-Boot counted each reset as a failed boot. `pm_test=core` (everything
  except the PSCI hand-off) returned fine and its resume log showed
  `imx93-blk-ctrl: failed to power up domain` in the noirq phase, followed by
  clock unbalance warnings and a runtime-PM underflow → patch 0017.
- With 0016 the deep path got through device, late and noirq suspend and was
  then aborted by `PM: Triggering wakeup from IRQ 106`, the lis2dw12 INT1
  line, which had also stormed at boot (`irq 106: nobody cared`) → edge
  trigger in patch 0016. The one-second s2idle wakes seen earlier were the
  same pending IRQ, not the power key latching its own press.
- Rebinding `st-accel-i2c` after resume (the vendor setup's unbind for the
  lis2dw12) produced `irq 108: nobody cared` → the mainline variant no
  longer unbinds it (an integration's unbind-across-suspend list must leave
  it out).
- Final build (edge-triggered INT1, suspend-debug symbols dropped again):
  no interrupt storm, `deep` is the default sleep mode, a 15.6 s deep sleep
  and a 25 s power-key suspend both woke on the next press with the rollback
  counters untouched. Both slots run it.

If resets return, the next suspect is the i.MX93 watchdog that U-Boot leaves
running: `imx7ulp_wdt` only parks it when userspace holds `/dev/watchdog0`,
which nothing does here at runtime.
