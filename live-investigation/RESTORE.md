# AS4610-54T — Stock ICOS NOS Backup & Restore

A complete, **verified, restorable** image of the factory Edgecore NOS (Broadcom
FASTPATH/ICOS 3.4.3.7) for the AS4610-54T (S/N EC2025000934). Use this to put the
original NOS back if the box is overwritten (e.g. by installing our own NOS).

> The image files live in **`backup/`** (git-ignored — proprietary Edgecore/
> Broadcom code, kept local, **not** pushed to GitHub). This guide is the doc.

## How it was taken (clean)

Imaged from **ONIE rescue** (`run onie_rescue` from U-Boot), where `/dev/sda` is
**unmounted** — so the filesystems are consistent (not a live/mounted copy).
Streamed `dd | gzip` over SSH to the capture host. Verified:
`e2fsck -fn` on `sda2` = **clean**; `image1`/`image2` present at 30,488,929 B each.
The accidental saved config was removed first, so the image is **pristine default**.

## Backup set (`backup/`, see `MD5SUMS`)

| File | What | Restore target |
|---|---|---|
| `sda.gpt` | GPT partition table (`sgdisk --backup`) | `/dev/sda` GPT |
| `sda_head.bin` | first 1 MiB of sda (protective MBR + primary GPT) | (redundant w/ sda.gpt) |
| `sda1.img.gz` | **ACCTON-DIAG** partition (256 MiB) — diag `uImage` (22 MB) | `/dev/sda1` |
| `sda2.img.gz` | **DCSS** partition (512 MiB) — **ICOS `image1`+`image2`** + default config | `/dev/sda2` |
| `mtd0_uboot.bin` | U-Boot (896 KiB, SPI) | `/dev/mtd0` |
| `mtd1_shmoo.bin` | DDR shmoo cal (64 KiB) | `/dev/mtd1` |
| `mtd2_env.bin` | U-Boot environment (64 KiB) | `/dev/mtd2` |
| `mtd3_onie.bin` | **ONIE** 2016.05.00.04 (7 MiB, SPI) | `/dev/mtd3` |
| `backup/uboot_env.txt` | human-readable U-Boot env (`fw_printenv`) | reference / `fw_setenv` |

Disk: 29.5 GiB GPT, GUID `6A9387CD-F534-419C-9BA2-A1E39309BE27`; only 2 small
partitions, 28.7 GiB free. Box-specific: MAC `04:F8:F8:15:A8:40`, serial
`EC2025000934` (both in the env). ONIE is the standard re-downloadable part
(`arm-accton_as4610_54-r0`).

## Restore procedure (from ONIE rescue)

1. **Boot ONIE rescue**: interrupt U-Boot autoboot, then `run onie_rescue`.
   (`tools/catch_uboot.py` automates catching the prompt over serial.)
2. **Get the backup onto the box.** ONIE has `ssh`/`scp` (legacy keys). On the
   capture host, re-add your key + copy files:
   ```sh
   # in ONIE shell (over serial), allow your key:
   mkdir -p /root/.ssh; echo '<your pubkey>' > /root/.ssh/authorized_keys
   # from host (legacy algos for old dropbear):
   SSHO='-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa'
   scp $SSHO backup/sda*.gz backup/sda.gpt backup/mtd2_env.bin root@<onie-ip>:/tmp/
   ```
   (or push from host directly into a restore pipe — see step 4.)
3. **Restore the partition table** (only if the GPT was wiped):
   ```sh
   sgdisk --load-backup=/tmp/sda.gpt /dev/sda     # exact original layout
   ```
4. **Restore the partitions** (the important step — the ICOS code lives in sda2):
   ```sh
   gunzip -c /tmp/sda1.img.gz | dd of=/dev/sda1 bs=1M
   gunzip -c /tmp/sda2.img.gz | dd of=/dev/sda2 bs=1M
   sync
   ```
   Or stream from the host without staging on the box:
   ```sh
   gunzip -c backup/sda2.img.gz | ssh $SSHO root@<onie-ip> 'dd of=/dev/sda2 bs=1M'
   ```
5. **Restore U-Boot env** (boot config: `active=image1`, dual-image, MAC). Either
   flash the raw image or re-set vars:
   ```sh
   dd if=/tmp/mtd2_env.bin of=/dev/mtd2 bs=64k        # raw env restore
   # (alternatively replay individual vars with fw_setenv from dumps/uboot_env.txt)
   ```
6. **U-Boot / ONIE (`mtd0`/`mtd3`) — usually NOT needed** (they're untouched by a
   NOS install, and ONIE self-heals / is re-downloadable). Only reflash if
   corrupted, and carefully (`flashcp`), since a bad U-Boot bricks the box:
   ```sh
   flashcp -v /tmp/mtd0_uboot.bin /dev/mtd0     # u-boot  (RISKY — only if needed)
   flashcp -v /tmp/mtd3_onie.bin  /dev/mtd3     # onie
   ```
7. **Reboot** → U-Boot `nos_bootcmd` boots `image1` → ICOS comes up at factory
   default. Login `admin` / (blank).

## Minimal restore (most cases)

If you only overwrote the OS (installed another NOS over ONIE), the original ICOS
comes back with just: **`sda.gpt` → `sda1` → `sda2` → `mtd2_env`** (steps 3–5).
U-Boot and ONIE are untouched by a normal ONIE-based NOS install.

## Verification on capture (already done)

```
e2fsck -fn sda2  ->  DCSS: 52/32768 files, 17103/131072 blocks  (clean)
debugfs stat /image1,/image2  ->  Size 30488929 each
gzip -t sda1.img.gz, sda2.img.gz  ->  OK ;  md5 in backup/MD5SUMS
```
