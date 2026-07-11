# Booting a custom OS (no UART, no signing)

This is OS-agnostic: it's how the device's bootloader finds and starts a kernel. It
works for any Linux distribution — you just need a kernel + rootfs you can package
into the FIT the bootloader expects.

## Prerequisite: Developer Mode

You must enable **Developer Mode** on the device (in the vendor OS settings)
*before* any of this works. Enabling it moves the AHAB lifecycle to **OEM Open**
(`unlocked=yes`), which is what makes U-Boot skip signature verification and boot
unsigned FIT images. Without it, U-Boot rejects any image you build here.

Developer Mode is a one-way transition and **wipes user data** when enabled, so
do it before you have anything on the device you care about. See reMarkable's
[Developer Mode documentation](https://developer.remarkable.com/documentation/developer-mode).
It also reveals the SSH/root password (Settings → Help → About), which you'll
need for deploys.

## TL;DR

1. The unit boots **unsigned** images because Developer Mode put it in
   **OEM-Open / `unlocked=yes`** (U-Boot skips AHAB auth). No keys/fuses needed.
2. There are **two symmetric rootfs slots** (A=`mmcblk0p2`, B=`mmcblk0p3`). U-Boot loads
   `/boot/fitImage.ahab` **from the active slot's rootfs**.
3. The **active slot is stored in the `lpgpr` SNVS registers**, not in the eMMC
   boot partition. U-Boot reads it there (and honours the per-slot error
   counter). The stock `rootdev` tool reads/writes it; `rootdev --next-boot` is
   the authoritative "what boots next". The eMMC boot partitions (`boot0`/`boot1`)
   just hold `imx-boot` — toggling them with `mmc bootpart` does **not** change
   the rootfs slot.
4. So: put your OS on the **inactive** slot, point `lpgpr` at it (`rootdev
   --switch`), reboot.

## The FIT image (`fitImage.ahab`)

U-Boot loads `/boot/fitImage.ahab` from the active rootfs and boots it. Format:

```
[ 8 KB AHAB pad ][ FIT (kernel + DTB(s) [+ ramdisk]) ]
```

The FIT must parse at offset `0x2000` — so prepend an 8 KB pad (reuse the pad from a
vendor `fitImage.ahab`). Kernel load addr `0x80400000`, FIT addr `0x90000000`.

Build it:
```sh
mkimage -f your.its your.itb
head -c 8192 vendor/fitImage.ahab > fitImage.ahab   # reuse 8KB AHAB pad
cat your.itb >> fitImage.ahab
```
Include the **device tree for your PCBA revision** (read your unit's revision from the
vendor OS). No signing needed while `unlocked=yes`.

## A/B slots & the error counter

U-Boot keeps a per-slot error counter in SNVS:
`/sys/devices/platform/lpgpr/{roota_errcnt,rootb_errcnt}`.

- Each boot **increments** the active slot's counter; a healthy OS **resets** it.
- At the limit (**3**) U-Boot marks the slot bad and boots the *other* one
  (auto-rollback — your safety net while experimenting).
- **A fresh custom OS won't reset it**, so it climbs and rolls back after
  ~3 boots. Until you add a reset-on-boot service, reset it manually before each boot
  (from a working OS): `echo 0 > /sys/devices/platform/lpgpr/rootb_errcnt`.

## Switching the active slot (userspace, no UART)

The active slot lives in the `lpgpr` SNVS registers, and the stock OS's
`/usr/sbin/rootdev` is the tool that manages it:

```sh
rootdev --active      # rootfs the running system booted from
rootdev --inactive    # the other slot (deploy target)
rootdev --next-boot   # what U-Boot will boot next — authoritative
rootdev --switch      # flip active/inactive for the next boot
```

`rootdev --switch` persists the choice via `lpgpr` (it does **not** simply write
the read-only `root_part` sysfs — that always reflects the *currently booted*
slot). Confirm the target with `rootdev --next-boot` before rebooting.

> **Not** `mmc bootpart`: the enabled eMMC boot partition (`boot0`/`boot1`) only
> selects which copy of `imx-boot` runs — both then read `lpgpr` for the rootfs
> slot — so `mmc bootpart enable …` does not switch which OS boots. Use `rootdev`.

## Deploy recipe (generic)

From a working OS (vendor on the other slot, or a prior custom OS):
```sh
# 1) write your rootfs to the INACTIVE slot (never the running one!)
mkfs.ext4 /dev/mmcblk0pX && mount /dev/mmcblk0pX /mnt
#    ...unpack your rootfs into /mnt...
# 2) put your FIT where U-Boot looks:
cp fitImage.ahab /mnt/boot/fitImage.ahab
# 3) point root= at that slot in your kernel cmdline / FIT bootargs
# 4) reset that slot's error counter so U-Boot will try it:
echo 0 > /sys/devices/platform/lpgpr/rootX_errcnt
# 5) point lpgpr at that slot + reboot
rootdev --switch          # then verify: rootdev --next-boot
sync && reboot
```

### Gotchas learned the hard way
- **Never `mkfs`/write the *active* (running) slot** — it freezes the device.
- A distro initramfs that expects its own boot-partition layout won't fit this
  whole-partition A/B scheme and may panic-loop. The reliable method is a
  **no-ramdisk** boot (kernel mounts `root=` directly).
- USB networking: the gadget is **CDC ECM** (device `10.11.99.1`, holds its address
  on its own — the device side needs nothing). All the flakiness is host-side, from
  one fact: **the gadget gets a new interface NAME on every enumeration** (device
  reboot, replug, SDP round-trip) because the name is USB-path-derived
  (`enp0s20f0u7`, `enp0s20f0u5u1u2`, …). Two failure modes follow, both host-only:
  - **Stale duplicate address.** If you manually `ip addr add 10.11.99.2/24` to the
    live iface, the address lingers on the previous (now-dead) iface too. Two ifaces
    with `10.11.99.2/24` = duplicate connected route; the kernel may send device
    traffic out the dead one (`ip route get 10.11.99.1` reveals it). Delete the stale
    address (or don't hand-assign — see below).
  - **Admin-down on reappear.** A freshly re-enumerated iface can come back down with
    no address; the device looks dead while it's up. Check `lsusb` for the composite
    gadget; if present, the link is fine and it's purely host config.
- **The clean fix (do this once, stop hand-fixing):** let your host's network manager
  own `10.11.99.2/24`, matched by the *stable* CDC ECM **driver** rather than the
  shifting name, so any enumeration gets exactly one config and the address is removed
  when the link goes away. With systemd-networkd:
  ```ini
  # /etc/systemd/network/30-remarkable-usb.network
  [Match]
  Driver=cdc_ether
  [Network]
  Address=10.11.99.2/24
  ```
  On NetworkManager, match a name glob instead (`[match] interface-name=enp0s20f0u*`)
  or hand these ifaces to networkd. This makes the link self-healing across replugs
  and renames.
- If a custom boot gets stuck, **power-cycle ~3×**: the error counter hits the limit and
  U-Boot rolls back to the good vendor slot. Worst case: [recovery.md](recovery.md).
- U-Boot appends `dm-mod.create=`/`dm-mod.waitfor=` args that crypt-map **p4 by
  partition number** and hold it exclusively. If you repartition or want p4 for
  yourself, build with `CONFIG_DM_INIT=n` — see
  [hardware.md](hardware.md#u-boot-injected-kernel-cmdline-the-p4-dm-crypt-mystery).
- **Power button + connected USB = SDP mode.** Holding power while USB is plugged in
  drops the device into the boot-ROM serial downloader (`2edd:0140 "OC Blank 93"`) —
  it looks dead but isn't. Nothing is flashed by merely entering it. To exit:
  unplug USB, hold power to force off, short-press to boot, replug once booting.
- **No U-Boot env editing from userspace** — the vendor OS ships no `fw_setenv` /
  `fw_printenv`, so you cannot rewrite `rM_bootcmd`/`mmcpart`/etc. from Linux. The
  slot switch is done via the eMMC boot-partition enable + `lpgpr`, not U-Boot env.

### Why a kernel that won't boot gives you *nothing*

Debugging a non-booting custom kernel on this device is unusually hard, by design:
- **`STRICT_DEVMEM=y`** in the vendor kernel → `dd if=/dev/mem` of a RAM address
  returns `EPERM`. There is **no `/sys/fs/pstore` and no reserved `ramoops` region**,
  so you **cannot recover a panic by reading RAM back** from the (only-bootable)
  vendor OS. The only viable software exfil is kernel→eMMC, which needs the kernel to
  live far enough to write — hence the p1 marker/`INITRD_LOG` approach.
- **`IKCONFIG` is off** (no `/proc/config.gz`), so there's no embedded `.config` to
  diff against; compare your config against the vendor `modules.builtin` instead
  (261 drivers `=y`).
- With UART needing a full teardown ([hardware.md](hardware.md#serial-console-uart)),
  the realistic bring-up channels are all USB: the stage-1 USB-ECM beacon
  (kernel-alive proof) and the p1 crash-log marker.

## Boot-counter reset timing (important)

Reset the slot's error counter **as late as possible in boot** — after
networking/SSH are up — never in an early boot service. The reset is the
"boot succeeded" signal; resetting early means a mid-boot failure still counts
as success and U-Boot's auto-rollback will never trigger. See
[power.md](power.md) for the failure mode this causes with a flat battery.
