# Recovery / unbrick

Your safety net. The device has a boot-ROM serial-download path that is **independent
of the A/B slots and the boot counter**, so even a fully broken eMMC bootloader can be
revived. Know this before you start flashing.

## Two layers of safety

1. **A/B auto-rollback (soft).** If a custom OS fails to boot ~3 times, U-Boot marks
   the slot bad and boots the other (vendor) slot. Often a few power-cycles is all you
   need. See [booting-a-custom-os.md](booting-a-custom-os.md).

2. **NXP SDP recovery (hard).** Hold power + connect USB to enter the i.MX boot-ROM
   serial-download mode; the device enumerates as an NXP SDP gadget
   (`2edd:0140`, "OC Blank 93"). reMarkable's `rm_recover` (which bundles NXP's `uuu`)
   then loads `imx-boot` over USB, drops U-Boot into fastboot, and reflashes eMMC.

## Using `rm_recover`

- It's reMarkable's own tool; we don't redistribute it. Get it from reMarkable:
  - **Linux**: <https://developer.remarkable.com/documentation/recovery-for-linux-host>
  - **macOS**: <https://support.remarkable.com/s/article/Software-recovery>
  - **Windows**: the recovery flow goes through the reMarkable desktop app; we
    haven't traced how it fetches the image, so the Linux/macOS `rm_recover`
    path documented here is the known-good route.
- It pulls a per-device image from a **public** bucket
  (`device-recovery.cloud.remarkable.com`, machine `chiappa`); object URLs are computed
  at runtime by the tool.
- `rm_recover` is a dynamically linked x86-64 glibc binary; `TMPDIR=<dir>` pins where it stores
  the downloaded artifacts (handy — that's also how you get the recovery rootfs for
  [obtaining-vendor-blobs.md](obtaining-vendor-blobs.md)).
- Enter SDP: **long power hold (~30 s) → release → short power hold (~3 s)**.
  The device then enumerates as `2edd:0140 "OC Blank 93"`. USB does not need to be
  (re)connected for the button sequence — you can leave it plugged the whole time.
  (Observed 2026-07-06; more reliable than a single long hold.)

## Good news on re-locking

Running the vendor recovery does **not** re-lock the device — the AHAB lifecycle is a
one-way fuse and reMarkable's recovery scripts don't run `ahab_close`/`ahab_lock`. So a
recovery flash restores the stock OS without taking away your ability to boot unsigned
images. (An earlier note here suggested re-asserting `unlocked=yes` via fastboot
`ucmd` — the production imx-boot rejects `ucmd`, see below; the `unlocked` env is
derived from the fuse lifecycle at normal boot, so no action should be needed.)

## Stock libuuu works (no rm_recover needed for SDP access)

Stock libuuu (tested 1.5.243, e.g. `nixpkgs#uuu`) can drive the device if you map
the custom USB IDs with `CFG:` lines at the top of the script — see
[`recovery/uuu-scripts/sdp-to-fastboot.uuu`](../recovery/uuu-scripts/sdp-to-fastboot.uuu):

```
CFG: SDPS: -chip MX93 -compatible MX815 -vid 0x2edd -pid 0x0140
CFG: SDPV: -chip SPL1 -compatible SPL -vid 0x2edd -pid 0x0141
CFG: FB: -vid 0x2edd -pid 0x0142
```

The `-compatible` entries are required (libuuu resolves ROM quirks through them;
`-chip` alone fails "can't get rom info"). Two more stock-uuu gotchas: it does
NOT substitute `_placeholder` args in user scripts (only in built-in `-b`
scripts), and it resolves `-f` file paths relative to the script's directory —
so copy the imx-boot next to the script as `imxboot.bin`.

## ⚠️ "oem unlock" is the dev-mode wipe — you don't need it to flash

Confirmed from reMarkable's published U-Boot source (github.com/reMarkable/
**uboot-imx-rm**, branch `rmpp_v2025.04_v3.26.x`, tarball via git-lfs;
`board/reMarkable/common/imx93/{imx93_fastboot.c,unlock.c}`):

- `FB: oem unlock` on an already-unlocked (dev-mode) device just answers
  `already unlocked` (uuu renders it as a Fail) and touches nothing.
- On a **locked** device it writes `REBOOT_UNLOCK` into BBNSM_GPR0 and resets;
  on the next boot secure SPL prompts *"Please, double-press the power key to
  unlock"* (30 s) and, on the double-press, runs **`factory_reset()` — wiping
  keys and user data** — before persisting the unlocked state. It is exactly
  the Developer-Mode enable, physical-presence check and wipe included.
- **Flashing is never gated by it**: this build has `CONFIG_FASTBOOT_LOCK`
  off, so fastboot `flash`/`erase` carry no lock check at all. (That's also
  why `getvar unlocked` is a hardcoded `"no"` — it reads a compiled-out
  subsystem, not the BBNSM state. Ignore it.)

## Fastboot over SDP: the `ucmd` allowlist and the `fastboot_dev` trick

Everything that confused us on 2026-07-05 has a two-line explanation in the
source (`board/reMarkable/chiappa/chiappa.c:269-302`,
`drivers/fastboot/fb_fsl/fb_fsl_common.c`):

1. **`ucmd` only accepts an exact-string allowlist** ("required by uuu"); any
   other command returns a bare `Fail` with no message. The list includes
   `reset`, `poweroff`, `mmc dev 0`, `setenv mmcdev ${emmc_dev}`,
   `setenv fastboot_dev mmc`, `mmc partconf ${emmc_dev} ${emmc_ack} 1 0`,
   `mmc bootbus 0 2 1 2`, `mmc part`, `env print serial#|unlocked`, and a few
   fuse reads — matched with `strcmp`, so they must be sent **verbatim**
   (env-variable form included). `oem run` doesn't exist in this backend.
2. **The USB/SDP boot path never sets the `fastboot_dev` env var**, so the
   fastboot partition table starts empty — every partition name (including
   `gpt`/`all`/`bootloader`) fails "Wrong partition name." and every flash
   fails "failed to flash device", even with a perfectly healthy GPT.
   The allowlisted `ucmd setenv fastboot_dev mmc` fixes it: after every
   successful ucmd, U-Boot re-runs `fastboot_setup()`, which now reads the
   real eMMC GPT into the table. From then on `getvar partition-size:<name>`,
   `flash <name>`, `flash -raw2sparse <name>`, and `flash gpt` all work.

### Flashing a slot over SDP with stock libuuu (verified recipe)

See [`recovery/uuu-scripts/sdp-flash-root-a.uuu`](../recovery/uuu-scripts/sdp-flash-root-a.uuu):

```
FB: ucmd setenv mmcdev ${emmc_dev}
FB: ucmd mmc dev 0
FB: ucmd setenv fastboot_dev mmc          # <-- populates the partition table
FB: getvar partition-size:root_a          # sanity: must return a size now
FB: ucmd mmc partconf ${emmc_dev} ${emmc_ack} 1 0   # boot select -> slot A
FB[-t 1800000]: flash -raw2sparse root_a rootfs.img
FB: reboot
```

**Slot-A-only caveat**: the allowlist contains only the *slot A* partconf
string — fastboot cannot point the boot select at slot B. So over SDP, deploy
to A (in SDP mode nothing is mounted, so writing the "active" slot is safe);
slot B deploys happen over SSH from the booted system.

## Non-destructive slot switch over SDP (no flashing)

If the device only boots a broken slot (e.g. the boot-counter reset ran before
the failure point, so U-Boot never rolls back), switch the boot select to
slot A from SDP without flashing anything (the partconf/bootbus strings above
are allowlisted for exactly this). Only the eMMC EXT_CSD boot-partition-select
bit changes.

1. Enter SDP: long power hold (~30 s) → release → short hold (~3 s); device shows
   as `2edd:0140 "OC Blank 93"` (USB can stay plugged throughout).
2. Obtain reMarkable's uuu + imx-boot: run `rm_recover --uuu /bin/false restore`
   once with `TMPDIR` set — it downloads all artifacts, then fails harmlessly
   before flashing. (Run once more without `--uuu` to also fetch their uuu
   binary; unprivileged it fails at USB open, after the download.)
3. Run the slot switch (as root):
   ```sh
   sudo <artifacts>/uuu -b recovery/slot-switch.uuu <artifacts>/…-imx-boot
   ```
   `-b` is required for the `_imxboot` argument substitution. The script boots
   imx-boot from RAM, then issues `mmc partconf ${emmc_dev} ${emmc_ack} 1 0`
   (boot0 = slot A; use `2 0` … i.e. boot_partition=2 … for boot1/slot B) and
   resets. Only the eMMC EXT_CSD boot-partition-select bit changes.
   The final `ucmd reset` reports a USB I/O error — that's the device rebooting,
   which is success.

Note: `mmc partconf 0 …` with a literal device number fails — the eMMC is not
mmc dev 0 in this U-Boot; always use the `${emmc_dev}` environment variable.
