# Firmware Manifest — reMarkable Paper Pro Move (chiappa)

Files extracted from the reMarkable public production image.  
Run `extract.sh <image>` to re-extract from any firmware version.

## Source image

| Field | Value |
|-------|-------|
| Filename | `remarkable-production-image-3.27.1.0-chiappa-public.ext4.verity.gz` |
| Firmware version | 3.27.1.0 |
| Image sha256 | `0e525f81a7b7baad80938505d3d85958f104deedb73d22500108ae154a97ddb5` |
| Archived at | `firmware/images/` |

## Kernel firmware blobs (`firmware/lib/firmware/`)

These are loaded by the kernel at boot. Without them the corresponding
subsystem either fails silently or won't probe.

| File | Subsystem | sha256 |
|------|-----------|--------|
| `ctn730.fw` | NFC (ctn730 i²c) | `33d9133b2801e37d714995f8e09427ad7c30eb19e5a540714682efb7c0c3d00f` |
| `elants_spi.bin` | Touchscreen (Elan SPI) | `b8bab9de0df9c643621be333ed7b20e2f8fab6831a6dd6891b534757f0466ab7` |
| `marker-asic.bin` | Pen ASIC | `f205fc2ce9faa7f9c9d2803b08cd3a6522fe8ff3c8116ff63ae7ca55c0cae428` |
| `marker-mcu.bin` | Pen MCU | `240b28800a2664439467458d9fc2c154514988a9360cca1d9555c3510bd43091` |
| `nxp/rgpower.bin` | NXP power management | `cf92a5becfbb7547ced333c12d0fcc2809fc394e577174b6e3ca82106e676f6a` |
| `nxp/sd_w61x_v1.bin.se` | WiFi (NXP IW61x) | `e245a30c6377cdc6c142c31a3118ffb1ff9a0a9a2b53ad436c89e1468a10cc37` |
| `nxp/uartspi_n61x_v1.bin.se` | Bluetooth (NXP IW61x) | `2c86126faa1fdc25402ed6e982b794d07d016f4822bb807bd6a9ef5908475bf1` |
| `nxp/WlanCalData_ext.conf` | WiFi calibration data | `bcab0ae42dcc2b3689df048435a7ee259278ecc88b8c00ac93ba791474d43421` |
| `regulatory.db` | WiFi regulatory | `7f05b7d7321bf6fcadab3d93279ab27fb8be7cc63c556f1f1654c0cd2c6aeea2` |
| `regulatory.db.p7s` | Regulatory DB signature | `0a515b26e7fd1b29b6cac1a0696c9841039fe559a44f79cd4c67056547539216` |
| `spld_rev_f.hex` | SPLD (hw rev F) | `802260cac5d9ca86ece26d9df561dd54d86a251584d890c5e6ee0aec79a09430` |
| `spld_rev_g.hex` | SPLD (hw rev G) | `734be615ea2323a0cc629314a8e50a2be37a1002877d6b6f66fc4a22c61f591a` |
| `spld_rev_h.hex` | SPLD (hw rev H) | `734be615ea2323a0cc629314a8e50a2be37a1002877d6b6f66fc4a22c61f591a` |
| `spld_rev_i.hex` | SPLD (hw rev I) | `734be615ea2323a0cc629314a8e50a2be37a1002877d6b6f66fc4a22c61f591a` |
| `spld_rev_j.hex` | SPLD (hw rev J) | `734be615ea2323a0cc629314a8e50a2be37a1002877d6b6f66fc4a22c61f591a` |
| `spld_rev_k.hex` | SPLD (hw rev K) | `734be615ea2323a0cc629314a8e50a2be37a1002877d6b6f66fc4a22c61f591a` |
| `tee.bin` | OP-TEE trusted OS | `f2b523156f1a6bef513f41c6f8e3bde4c4583b6815b57658c238d5dbb402f98d` |
| `tee.elf` | OP-TEE ELF (debug) | `d874c5452de087314988d2d480d6f58446ec7e424de1a309cde3dcd21db90ab4` |
| `tee-header_v2.bin` | OP-TEE header | `4ea43a819596716c1caab24635225a47b91d3b827b9e3b6992585faeba577b48` |
| `tee-pageable_v2.bin` | OP-TEE pageable | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `tee-pager_v2.bin` | OP-TEE pager | `dd555f479ed5ceb430be088078f9225f9d5f66d9ef108b48d41a25e434a65010` |
| `tee-raw.bin` | OP-TEE raw | `dd555f479ed5ceb430be088078f9225f9d5f66d9ef108b48d41a25e434a65010` |

## E-ink waveforms (`firmware/waveforms/`)

Panel-specific waveform files. The filename encodes the panel lot ID
(`AAB0B7` etc.) and IC revision. At runtime, the SWTCON engine selects
the matching waveform based on the lot ID read from the panel EEPROM.
All `.eink` files are from `usr/share/remarkable/` in the firmware image.

| File | sha256 |
|------|--------|
| `colortable_best.bin` | `1c324b4e934829e972875e5b4575bc07c3182027eb9bc04f1f0907bbf89da626` |
| `colortable_fast.bin` | `1c324b4e934829e972875e5b4575bc07c3182027eb9bc04f1f0907bbf89da626` |
| `colortable_pen.bin` | `1c324b4e934829e972875e5b4575bc07c3182027eb9bc04f1f0907bbf89da626` |
| `colortable_std.bin` | `1c324b4e934829e972875e5b4575bc07c3182027eb9bc04f1f0907bbf89da626` |
| `ct33_best.bin` | `ceb48788ee73da7ed1f14ef96e04ddb96e78c3393b82027aa56c43154715f38b` |
| `ct33_fast.bin` | `ceb48788ee73da7ed1f14ef96e04ddb96e78c3393b82027aa56c43154715f38b` |
| `ct33_pen.bin` | `ceb48788ee73da7ed1f14ef96e04ddb96e78c3393b82027aa56c43154715f38b` |
| `ct33_std.bin` | `ceb48788ee73da7ed1f14ef96e04ddb96e78c3393b82027aa56c43154715f38b` |
| `GAL3_AAB01S_TF0203_AC073MC1F2_AD1004-GCA_TC.eink` | `5246d710b22039a9fda19ed5d9a9c05061563b4f01d441534659e4e65075fdc3` |
| `GAL3_AAB01T_TF0303_AC073MC1F2_AD1004-GCA_TC.eink` | `8b6198fd7b2346b9cb04eb980245ca6bdbdfd9dd592c2d85402f9698083aabc3` |
| `GAL3_AAB067_IC0401_AC073MC1F2_AD1004-GCA_TC.eink` | `b04601255fc4f348e6f0d7d1d39fde99267879da55e86f65e109c1bc49e9af49` |
| `GAL3_AAB0AD_IC0B01_AC073MC1F2_AD1004-GCA_TC.eink` | `0bb1d07a83defb9abd56fddedcf719167aa01d0dd84b9e581e1aeb4e529721a2` |
| `GAL3_AAB0AG_IC0301_AC073MC1F2_AD1004-GCA_TC.eink` | `454a2093607bf63f6ddfd13252c11bd3c8fc8865a94b20dfd2485dbca5c4bc46` |
| `GAL3_AAB0AH_IC0501_AC073MC1F2_AD1004-GCA_TC.eink` | `f639e8d972d1eaa7e5e325a021d3b64fafc06c802800b044aca07b3a60d51083` |
| `GAL3_AAB0AK_IC0701_AC073MC1F2_AD1004-GCA_TC.eink` | `83c36f40431396b5bd6aae0dcd9a1b3e679c7121b1e8982fa1150418fe6c1e23` |
| `GAL3_AAB0AL_IC0901_AC073MC1F2_AD1004-GCA_TC.eink` | `c1b7a1f38a9ca9048a0c3ffe82ed1dfce1c964c67f595aeffa4c6d517adcedc7` |
| `GAL3_AAB0AM_IC0801_AC073MC1F2_AD1004-GCA_TC.eink` | `80b8174773effceefbc16b54722cc0afd2187bd9a7c260a71bfbf92baeae8b67` |
| `GAL3_AAB0AN_IC0601_AC073MC1F2_AD1004-GCA_TC.eink` | `c17321dd565922a015466ce497a79920990fa07a85ce0da52fcd5848d066c19a` |
| `GAL3_AAB0AQ_IC0A02_AC073MC1F2_AD1004-GCA_TC.eink` | `187b940314862b230c0b2107d2c5ac023f55c3e6239ef14ef408e144b87242da` |
| `GAL3_AAB0AS_IC1901_AC073MC1F2_AD1004-GCA_TC.eink` | `c3025f8a5efea566bc58a9405b0b7da01d9f1183b1426a653276129559397f78` |
| `GAL3_AAB0AT_IC1A01_AC073MC1F2_AD1004-GCA_TC.eink` | `de16e2c1addcbf382d058dfdf079353d99fc8d00301b153fa9a6bc461ad41486` |
| `GAL3_AAB0AU_IC1B01_AC073MC1F2_AD1004-GCA_TC.eink` | `cea39cfba7ab94aa7defbcb8ce1b4db08e9316b760eff9766d2c82a99e764e26` |
| `GAL3_AAB0AX_IC2001_AC073MC1F2_AD1004-GCA_TC.eink` | `620a4e886359dcb0d1c8efd7267a3f311f37950cb1b65df891b7d4dde3c141d0` |
| `GAL3_AAB0B0_IC0C02_AC073MC1F2_AD1004-GCA_TC.eink` | `bd2fcce11e83896e5796369d05fede20ad68af713ace35c2b5873381eaadbca4` |
| `GAL3_AAB0B1_IC1601_AC073MC1F2_AD1004-GCA_TC.eink` | `032c0df838aa449be6b7b4b2331e4df2157caf9c539c7c53e5372b1e849337b3` |
| `GAL3_AAB0B2_IC1101_AC073MC1F2_AD1004-GCA_TC.eink` | `62a0129a82b0363387ba680778ca1e7eaff2e111ab0247652ef3c4a6ecbacf83` |
| `GAL3_AAB0B4_IC1201_AC073MC1F2_AD1004-GCA_TC.eink` | `0d65e2daf9d3a1f3930ee4c2208c5154af4f92d3e181b3e314411b70ea035caa` |
| `GAL3_AAB0B5_IC1702_AC073MC1F2_AD1004-GCA_TC.eink` | `4137e07c7252254b740e8829220cb62841cd95ca99c4e951405b11a53753e3c8` |
| `GAL3_AAB0B6_IC1801_AC073MC1F2_AD1004-GCA_TC.eink` | `13ff2f0241737f22c4e87e241ccd594773145c6a81aa7018ab1df6f15c0e717b` |
| `GAL3_AAB0B7_IC0E02_AC073MC1F2_AD1004-GCA_TC.eink` | `1f9628c5e80ba2e3971b50ba7f20abba0422624559b41700d8a1e96e3e950fdb` |
| `GAL3_AAB0B8_IC0F01_AC073MC1F2_AD1004-GCA_TC.eink` | `fb4a0f5d349366bf09df1e89f061330863753502389794812395f475799c74b8` |
| `GAL3_AAB0B9_IC0D01_AC073MC1F2_AD1004-GCA_TC.eink` | `297c6d8b373b97d57625524be4464c8aa95fbb1a1bdb61428e5c06f2c572b56d` |
| `GAL3_AAB0BA_IC1301_AC073MC1F2_AD1004-GCA_TC.eink` | `0503f091d99fdc120230e48cec70c78af212e293c496095695022d2b41341b3d` |
| `GAL3_AAB0BB_IC1001_AC073MC1F2_AD1004-GCA_TC.eink` | `2fdb7aaa4f71056bbaa86bf6e7b6e278ff182c9310e1dc460661315d0ca28878` |
| `GAL3_AAB0BC_IC1401_AC073MC1F2_AD1004-GCA_TC.eink` | `07fe649690a9569757e6d1f82cf590fa808c1b113e3c2e988d5c2190f8558cc1` |
| `GAL3_AAB0BG_IC1501_AC073MC1F2_AD1004-GCA_TC.eink` | `d487ee73a8ae49cf2b3553a79ca6049b99bd8d77638a203795dcac5d18fc7f00` |
| `GAL3_AAB0D2_IC1D02_AC073MC1F2_AD1004-GCA_TC.eink` | `d7285bd5c42b023b126b441992f6f45c18267f30764c929d287336fab0c1ffa2` |
| `GAL3_AAB0D3_IC1E01_AC073MC1F2_AD1004-GCA_TC.eink` | `ab8f017a3fd7c4b7fcb644b3b0cee0be9caafbf4db1ac1b070aae6c41d9e933b` |
| `GAL3_AAB0D4_IC1F01_AC073MC1F2_AD1004-GCA_TC.eink` | `2b43e4400d83fc313bea78f1718dd8bbb24dde520d27a07e985e5683ed7a2f24` |
| `GAL3_AAB0D5_IC1C01_AC073MC1F2_AD1004-GCA_TC.eink` | `f5e7bb435d2432827f05a9a1fe890659f97afcc715ff532168498992d8111cbb` |

## Qt6/epaper vendor bundle

The `einkbridge` binary links against the vendor's Qt6.8.2 + `libqsgepaper`
runtime. This is a proprietary closed-source library from reMarkable/E Ink;
it is **not** redistributable. You must extract it from your own device's
firmware image using `eink/scripts/build-bundle.sh`. The extracted bundle
lives at `/opt/eink/` on the target system.

See `eink/scripts/build-bundle.sh` for the full extraction procedure.
