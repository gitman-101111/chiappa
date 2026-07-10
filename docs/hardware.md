# Hardware — reMarkable Paper Pro Move (`chiappa`)

All figures below were read off an owned retail unit (codename **chiappa**;
`ferrari` is the larger plain Paper Pro). FCC ID **2AMK2-RM03AA**.

## SoC & core

| Component | Detail |
|---|---|
| SoC | NXP **i.MX 93** (imx93), 2× Cortex-A55 @ ~1.7 GHz |
| RAM | 2 GB LPDDR4 (Micron) |
| Storage | 64 GB eMMC, GPT (Kingston `EMMC64G-TY29`) |
| PMIC | NXP PCA9451A (LPI2C) |
| Charger | MAX77818 |
| Bootloader | U-Boot **2025.04** (NXP ELE/AHAB) |
| Vendor OS | "Codex Linux" (Yocto scarthgap), kernel 6.12.x i.MX |

## Peripherals

| Function | Part / driver |
|---|---|
| Display | custom reMarkable EPD panel — DT `remarkable,cumulus-panel`, over **LVDS → LCDIFv3** (`imx-lcdifv3`, `imx93-parallel-disp-fmt`, `panel-rm-cumulus`) |
| Touch | Elan SPI (`elants_spi`) |
| Wi-Fi / BT / NFC | NXP **IW611** combo (mwifiex-family Wi-Fi, NXP UART BT, NFC) |
| USB | USB-C OTG via `ci_hdrc_imx` (ChipIdea); gadget = **CDC ECM** (host side `10.11.99.1`) |
| Frontlight | AW99703 (I2C) |
| Accelerometer | ST LIS2DW12 (I2C) |
| Power key / RTC | NXP BBNSM |
| EPD bias | dedicated EPD PMIC + rails (VCOM, VPOS/VNEG/VGH/VGL/VPDD) |

## eMMC partition layout (from `/proc/partitions`)

```
mmcblk0p1    ~100 MB    vendor data/config
mmcblk0p2    ~4.0 GB    ext4   root_a     ┐ A/B rootfs slots (symmetric)
mmcblk0p3    ~4.0 GB    ext4   root_b     ┘
mmcblk0p4    ~1.6 GB    swap   (dm-crypt; key in SNVS "bootkey")
mmcblk0p5     ~48 GB    data/home
mmcblk0boot0   4 MB     bootloader (imx-boot) ┐ A/B
mmcblk0boot1   4 MB     bootloader (imx-boot) ┘
```

- A/B is **symmetric**; always deploy to the **inactive** slot, never the running one.
  (`mkfs` of the live root will hang the device.)
- The **active slot is selected by which eMMC boot partition is enabled**
  (`mmcblk0boot0` ⇒ slot A, `mmcblk0boot1` ⇒ slot B). See
  [booting-a-custom-os.md](booting-a-custom-os.md).

## Serial console (UART)

- Device `ttyLP1`, **1.8 V**, **115200 8N1**. Pads are on `Chiappa_main_board RevG`
  as **exposed gold test pads but UNLABELED** (schematic is filed confidential) —
  identified via FCC internal-photo recon (FCC ID `2AMK2-RM03AA`). Reaching them
  needs a **full teardown** (screen + EMI shields + battery out). This is why every
  bring-up channel here is USB-based: there is no easy console.

## Boot chain

```
ELE (S400) → SPL → ATF (BL31) → U-Boot 2025.04 → rM_bootcmd
```

`rM_bootcmd` = `run loadimage; run authimage; run mmcboot; sleep 5; panic "boot failed"`:
- `loadimage` = `load mmc 0:${mmcpart} 0x8FFFE000 /boot/fitImage.ahab` — stages the
  FIT at **`0x8FFFE000`** from the active slot's rootfs (the AHAB container then maps
  it to `0x90000000` for parsing).
- `authimage` is **skipped** when `unlocked=yes` (this unit) — otherwise AHAB verify.
- `mmcboot` boots the FIT (kernel + per-revision DTB + optional ramdisk).
  Kernel load `0x80400000`, fit addr `0x90000000`.

## U-Boot-injected kernel cmdline (the p4 dm-crypt "mystery")

U-Boot never touches dm-crypt itself — its `mmcboot` sequence (`run mmcverity; run
mmcswap; run mmcargs; bootm …`) **composes a kernel command line** and hands it to
whatever kernel boots, including yours. Decoded from the vendor `imx-boot` env
(`strings` on the binary):

```
swappart=4
swapsize=3145728                      # 512-byte sectors = 1.5 GiB
swapkey=:32:logon:lpgpr:bootkey
mmcswap=setenv swap_map swap-encrypted-disk,,1,rw,0 ${swapsize} crypt
        aes-xts-plain64 ${swapkey} 0 /dev/mmcblk${mmcdev}p${swappart} 0 0
mmcargs=setenv bootargs console=${console} root=${rootdev} rootwait=5 ${quiet} ro
        rootfstype=ext4 panic=2 trusted.source=tee resume=/dev/dm-1
        systemd.gpt_auto=no fsck.repair=yes ${rescue} ${secargs}
        dm-mod.waitfor='"PARTLABEL=root_a"' dm-mod.create='"'${swap_map}${verity_map}'"'
```

So **every boot**, any kernel with `CONFIG_DM_INIT=y` (the vendor config has it):

- creates a rw dm-crypt mapping over the first 1.5 GiB of **`/dev/mmcblk0p4`** —
  `swappart=4` is hardcoded by partition *number*, not label;
- holds p4 open **exclusively** — a direct `mount`/`mkfs` of p4 fails `EBUSY`;
- blocks dm creation until `PARTLABEL=root_a` appears (`dm-mod.waitfor`) — don't
  lose that GPT label when repartitioning.

The 32-byte key is a kernel-keyring "logon" key named `lpgpr:bootkey`, created from
SNVS by the vendor driver `drivers/platform/remarkable/lpgpr.c` in the BSP tree.
When `unlocked=yes` there is no verity map, so the swap mapping is `dm-0` and the
hardcoded `resume=/dev/dm-1` dangles (harmless — the kernel's late resume attempt
just fails). When the error counter trips, U-Boot also injects rescue bootargs via
`${rescue}`.

**If you repartition** (e.g. delete the vendor swap and put data on p4), your kernel
must ignore these args or p4 becomes unmountable and writes through the stale
mapping scramble the head of your filesystem. Cleanest fix: build your kernel with
`CONFIG_DM_INIT=n` — the kernel then ignores `dm-mod.*` entirely, and dm-crypt/verity
remain available from userspace. The vendor slot boots its own kernel and keeps its
encrypted swap regardless.

## Security / lifecycle

The single U-Boot env var `unlocked` gates auth + rootfs integrity:

| `unlocked` | signature auth | rootfs |
|---|---|---|
| `yes` (retail unit observed) | **skipped** | plain `root=/dev/mmcblk0pN` |
| `no` | enforced (AHAB) | dm-verity (ro) |

`unlocked` follows the one-way AHAB fuse lifecycle (OEM-Open → Closed → Locked). The
observed retail unit is **OEM-Open**, confirmed by a plain `root=` in `/proc/cmdline`.
Running the vendor recovery does **not** re-lock it. This is why unsigned images boot.

## PCBA revisions

The vendor kernel ships several near-identical device trees (revisions F–K); U-Boot
selects one by detected PCBA revision. For a custom kernel you generally only need the
revision matching your unit (read it from the running vendor system).
