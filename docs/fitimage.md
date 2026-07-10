# FIT image structure and rebuild notes

## Vendor fitImage layout

The vendor boot image (`/boot/fitImage.ahab`) is an NXP AHAB container wrapping
a standard U-Boot FIT image.

```
Offset 0x0000 – 0x1FFF  : AHAB container (8192 bytes, signed by reMarkable SRK)
Offset 0x2000 – EOF      : FIT image (standard FDT/FIT format, magic 0xd00dfeed)
```

### Extracting pieces from a vendor fitImage.ahab

A custom-OS build needs two artifacts out of the vendor file (get it from your
device's `/boot` or the recovery image — see
[obtaining-vendor-blobs.md](obtaining-vendor-blobs.md)).
`firmware/extract-vendor-data.sh` does all of the below automatically; the
manual steps:

```sh
# the 8 KB AHAB header (prepended verbatim to any FIT you build):
dd if=vendor-fitImage.ahab of=ahab-header.bin bs=8192 count=1

# the bare FIT (for dumpimage), then the vendor DTB out of it — use the
# fdt image index matching your PCBA revision from `dumpimage -l`:
dd if=vendor-fitImage.ahab of=vendor-fit.itb bs=8192 skip=1
dumpimage -l vendor-fit.itb
dumpimage -T flat_dt -p <fdt-image-index> -o vendor.dtb vendor-fit.itb
```

The vendor DTB matters: the kernel-tree chiappa DTB has hung this device early
in boot; every boot known to work used the vendor one.

FIT contents (from `dumpimage -l`):

```
Description : Kernel fitImage for Codex Linux/6.12.49+git/imx93-chiappa
Created     : Thu Feb  5 03:35:18 2026

Image 0  kernel-1
  Type         : Kernel Image (ARM64 Linux, uncompressed)
  Load address : 0x80400000
  Entry point  : 0x80400000
  Size         : 13,850,632 bytes (~13.2 MiB)
  Hash         : sha256:275ccc2b...

Images 1-6   fdt-freescale_chiappa-rev-{f,g,h,i,j,k}.dtb
  Type        : Flat Device Tree
  Size        : 53,188 bytes each
  Hash        : sha256:5db0b36f... (all six are IDENTICAL in this firmware)

Configurations 0-5  conf-freescale_chiappa-rev-{f,g,h,i,j,k}.dtb
  Each config : kernel-1 + matching fdt
  No initrd/ramdisk
  Default     : conf-freescale_chiappa-rev-f.dtb
  (U-Boot overrides the default with the detected PCBA revision)
```

## Building a new FIT + AHAB image (custom kernel)

This requires the NXP AHAB signing toolchain and reMarkable's SRK keys (not
public). However, when `unlocked=yes` in the U-Boot environment, `auth_cntr`
is skipped, so an **unsigned** FIT image will boot fine.

### 1. Build the kernel

```sh
# Extract vendor source
tar -xzf linux-imx-rel-5.6-vc-3.26.0.68-122eda1b63d9.tar.gz
cd linux-imx-rel-5.6-vc-3.26.0.68-122eda1b63d9

# Start from vendor defconfig, then merge your distro's config fragment
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- chiappa_defconfig
scripts/kconfig/merge_config.sh .config /path/to/config-remarkable-chiappa.aarch64
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image dtbs modules
```

Output kernel: `arch/arm64/boot/Image` (uncompressed, ~13 MiB)

### 2. Build the FIT image (ITS template)

```its
/dts-v1/;
/ {
    description = "chiappa kernel";
    #address-cells = <1>;

    images {
        kernel-1 {
            description = "Linux kernel";
            data = /incbin/("arch/arm64/boot/Image");
            type = "kernel";
            arch = "arm64";
            os = "linux";
            compression = "none";
            load = <0x80400000>;
            entry = <0x80400000>;
            hash-1 { algo = "sha256"; };
        };
        fdt-chiappa-rev-h {
            description = "Chiappa rev H DTB";
            data = /incbin/("arch/arm64/boot/dts/freescale/chiappa-rev-h.dtb");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
            hash-1 { algo = "sha256"; };
        };
    };

    configurations {
        default = "conf-chiappa-rev-h";
        conf-chiappa-rev-h {
            description = "chiappa rev H";
            kernel = "kernel-1";
            fdt = "fdt-chiappa-rev-h";
            hash-1 { algo = "sha256"; };
        };
    };
};
```

```sh
mkimage -f chiappa.its chiappa.fit
```

### 3. Wrap in AHAB container for unlocked devices

When the device is unlocked (`unlocked=yes`), the AHAB wrapper is not verified,
but U-Boot still expects the file to be parseable as a container. Use
`mkimage_imx8` (from NXP's imx-mkimage toolchain) to create a minimal container:

```sh
# From the NXP imx-mkimage repo, IMX9 target:
mkimage_imx8 -soc IMX9 -c -ap chiappa.fit a35 0x90000000 \
    -out fitImage.ahab
```

Load address `0x90000000` is where the AHAB container maps the FIT for U-Boot
to parse; U-Boot then copies the kernel to `0x80400000` before booting it.

### 4. Deploy

```sh
# Assuming the target rootfs is on root_a (mmcblk0p2) and SSH is up:
scp fitImage.ahab root@10.11.99.1:/mnt/root-a/boot/fitImage.ahab
```

U-Boot loads `/boot/fitImage.ahab` from `mmcblk0p${mmcpart}` — make sure the
file is at that path on whichever partition you're booting from.

## Notes

- The i.MX 93 requires `mkimage_imx8` (not `mkimage_imx8m`) — specify `IMX9`.
- The a35 architecture flag refers to the Cortex-A35/A55 family used by IMX9.
- `0x90000000` is the staging load address used by the vendor build (confirmed
  from the README in reMarkable/linux-imx-rm).
- Without a signed AHAB container, only the `unlocked=yes` path works.  To sign
  for a locked device you need the SRK hash burned into the ELE fuses — don't
  attempt this unless you have a full backup strategy.
