# Mainline Linux feasibility (researched 2026-07-05, mainline ~7.1)

**Verdict: a hybrid "mainline + ~15-patch small patchset" tree is very achievable,
and would be the first public one for this device.** The i.MX93 SoC side is
essentially complete upstream; the work is ~5-8k LOC of self-contained leaf-driver
ports. One genuine blocker rides out-of-tree: WiFi. Current strategy stays on the
vendor 6.12.49 BSP; this doc is the map for the v2 track.

## SoC blocks (list A) — essentially done upstream

`imx91_93_common.dtsi`/`imx93.dtsi` on master cover: ccm, iomuxc, gpio, lpi2c,
lpuart, lpspi, usdhc, chipidea USB, fec/eqos, edma3/4, wdt, anatop, src/power
domains, mu + mu-s4, bbnsm (rtc + pwrkey, since 6.4), ocotp (imx-ocotp-ele),
ddr-pmu, media-blk-ctrl, lcdif, sai/micfil/xcvr/mqs, flexcan, fspi, i3c,
sysctr-timer. PCA9451A PMIC: pca9450-regulator.c.

Display: `fsl,imx93-lcdif` in mxsfb/lcdif_drv.c since v6.5; `fsl,imx93-ldb` in
bridge/fsl-ldb.c since v6.3 (it programs the LVDS PHY itself — the vendor's
separate lvds-phy node needs no mainline counterpart). Master's imx93.dtsi has
no LDB node yet — the board DT must add it (bindings exist; imx8mp shows the
pattern).

Gaps: EdgeLock "se" driver unmerged (v20 on list, Dec 2025 — NOT needed to
boot; fuse reads work via imx-ocotp-ele); no BBNSM **GPR** nvmem upstream (the
A/B slot state lives there → port `remarkable,lpgpr`, mandatory); FlexIO
i2c-master vendor-only; `nxp,imx93-lpm` (power scaling) vendor-only; PXP has no
imx93 support (we don't use it — e-ink pipeline is userspace/VKMS).

## Peripherals (list B)

| Device | Upstream? | Port cost |
|---|---|---|
| elants **SPI** touch | no (only elants_i2c) | moderate; carries the runtime-PM resume bug |
| AW99703 backlight | no, but sibling aw99706.c landed 2025 | easy (port or extend) |
| MAX77818 charger+fuel gauge | confirmed NOT upstream | moderate-hard; biggest B item; max17042 family partially reusable |
| FUSB303B Type-C | no (only fusb302; 303B is autonomous non-PD — TCPM wrong model anyway) | easy-moderate; gadget works without it |
| SLG46824 GreenPAK wake | no | small vendor port |
| LIS2DW12 accel | YES (IIO st_accel) | free |
| NTC thermistor | YES | free |
| G2194 regulator | no | tiny |
| **IW611 WiFi** | **NOT merged** — nxpwifi v9 (2026-02) still in review on linux-wireless | hard in-tree; moderate out-of-tree (nxp-imx/mwifiex-iw612) |
| IW611 Bluetooth (UART) | YES — DT is `nxp,88w8987-bt`, mainline btnxpuart binds | likely day-1 |
| CTN730 NFC | no (kernel NFC subsystem moribund) | port vendor ctn730_rm or skip |

## reMarkable-specific (list C) — small, self-contained (~2-3k LOC total)

- `panel-rm-cumulus` — 433-line DRM LVDS panel (copy in `kernel/reference/`);
  attaches to fsl-ldb → lcdif, both upstream. Trivial port.
- `remarkable,lpgpr` — **mandatory** (A/B slot select + per-slot boot error
  counters U-Boot honors + the dm-crypt bootkey logon key). ~300-600 LOC
  against BBNSM GPR.
- bootlog, lptmr-btc, sleep-monitor, suspend-event, pcba-bomrev, slg46824
  wakeup/gpio — optional diagnostics/wake plumbing, ~100-400 LOC each.

## Day-1 vs later

Day-1 on mainline defconfig + minimal board DT: boots to userspace, eMMC,
USB CDC-ECM gadget networking (the validation path — no UART), Bluetooth,
sensors, RTC/power key. First moderate tranche: display (add LDB node + panel
port; reuse the existing VKMS/writeback userspace pipeline) and touch. WiFi
out-of-tree until nxpwifi merges. Main quality risk: suspend/idle battery life
without the vendor sleep/LPM infrastructure — an e-reader lives on standby.

## Community landscape (2026-07)

No postmarketOS device page, no public mainline trees for ferrari/chiappa; only
reMarkable's BSP tarballs (git-lfs, not browsable trees). Closest prior art:
alistair23's rM2-mainline (i.MX7 — different SoC, proves the community model).
This repo appears to be ahead of anything public.

Sources (verified against torvalds/linux master 2026-07-05):
LCDIF imx93: lore.kernel.org/lkml/20230123072358.1060670-1-victor.liu@nxp.com;
fsl-ldb imx93: drm-misc 48865413c9dd; imx-se v20: patchew.org/linux/20251203-imx-se-if-v20;
nxpwifi v9: lwn.net/Articles/1057287; mwifiex-iw612: github.com/nxp-imx/mwifiex-iw612;
aw99706: lkml.org/lkml/2025/10/28/949; rM2 prior art: github.com/alistair23/linux (rM2-mainline).
