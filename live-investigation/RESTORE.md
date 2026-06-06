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

---

# VERIFIED end-to-end walkthrough: ONL/edged → ICOS (2026-06-06)

The steps above are correct in outline but **left out several real-world gotchas**
that bit us doing the actual restore from a *running EdgeNOS/ONL install* (not from a
pristine ONIE install). This section is the **tested, exact procedure** with every
trap called out. Serial recovery console throughout: **`/dev/ttyUSB1` @115200**
(helpers `tools/catch_uboot.py`, `tools/sercmd.py`). Logins: ONL `root`/`onl`,
ONIE `root` (key only), ICOS `admin`/blank then `enable`/blank.

> Context: the ONL installer **repartitioned the USB disk to an MBR table** with 4
> partitions (ONL-BOOT/CONFIG/IMAGES/DATA) and rewrote U-Boot `nos_bootcmd` to boot
> the ONL `.itb`. The original ICOS GPT (2 partitions) was gone — so this is a full
> restore, not a boot toggle.

## 0. Secure the return path FIRST (so you can get back to edged)
Before wiping anything, pull the exact running image + confirm the backup:
```sh
# from capture host — exact ONL image currently booted (return-to-edged artifact):
sshpass -p onl scp root@10.1.1.209:/mnt/onl/images/ONL-*_ARMHF.swi \
        backup/ONL-edgenos-4610-2026-06-04.swi
( cd backup && md5sum -c MD5SUMS )      # ICOS backup integrity
ls nos/datapath/mdk-app/edged            # the daemon binary to re-push
```

## 1. Get into ONIE rescue
```sh
# trigger reboot from the running ONL (over ssh), THEN catch U-Boot over serial:
sshpass -p onl ssh root@10.1.1.209 'sync; nohup reboot >/dev/null 2>&1 &'
python3 tools/catch_uboot.py 70 ' '     # spams SPACE to interrupt autoboot
```
- **GOTCHA — U-Boot prompt is `accton_as4610-54->`.** It is *not* in
  `catch_uboot.py`'s PROMPTS list, so the tool prints `[TIMEOUT — no prompt detected]`
  even though it **did** interrupt autoboot — the `accton_as4610-54->` prompt is sitting
  there. Just proceed.
- At the U-Boot prompt, enter rescue (send over serial with `sercmd.py`-style write):
  ```
  run onie_rescue
  ```
  → boots the ONIE ramdisk in **Rescue Mode** (installer disabled).

## 2. ONIE rescue networking + ssh access
- **ONIE got `10.1.1.209` via DHCPv4** on `eth0` automatically (there *is* a DHCP
  server on this net), and auto-started **dropbear ssh + telnetd**. If no DHCP, set a
  static IP over serial: `ifconfig eth0 10.1.1.209 netmask 255.255.255.0 up`.
- **GOTCHA — ONIE rescue is a fresh ramdisk every boot**: `/root/.ssh` is tmpfs and is
  **wiped on each ONIE boot**. Re-install your pubkey over serial each time:
  ```sh
  mkdir -p /root/.ssh; chmod 700 /root/.ssh
  echo '<your ssh-rsa pubkey>' > /root/.ssh/authorized_keys
  chmod 600 /root/.ssh/authorized_keys
  ```
  Use an **RSA** key — dropbear here is the 2017/legacy build.
- **GOTCHA — host→ONIE ssh needs legacy crypto flags:**
  ```sh
  SSHO='-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedKeyTypes=+ssh-rsa
        -o KexAlgorithms=+diffie-hellman-group1-sha1 -o StrictHostKeyChecking=no'
  ssh $SSHO -i ~/.ssh/id_rsa_switch root@10.1.1.209 'echo ok'
  ```

## 3. Transfer the backup to ONIE `/tmp`
- **GOTCHA — `scp` must use `-O` (legacy scp protocol).** ONIE dropbear has **no SFTP
  subsystem**; modern OpenSSH scp defaults to SFTP and fails with
  `subsystem request failed on channel 0 / Connection closed`.
  ```sh
  scp -O $SSHO -i ~/.ssh/id_rsa_switch \
      backup/sda.gpt backup/sda1.img.gz backup/sda2.img.gz backup/mtd2_env.bin \
      root@10.1.1.209:/tmp/
  ```
- ONIE `/tmp` is a ~1 GB tmpfs — the set (gpt + 22M + 58M + 64K) fits easily. Verify
  `md5sum` on the box against `backup/MD5SUMS`.

## 4. Restore GPT + partitions + env (the corrected step order)
ONIE has **only `sgdisk` + `dd` + `gzip`** for this — **no `partprobe`, `partx`,
`blockdev`, `flashcp`, `flash_erase`, or `fw_setenv`.**
```sh
# (a) GOTCHA: ONL left a valid MBR -> sgdisk --load-backup REFUSES it
#     ("Non-GPT disk; not saving changes. Use -g to override.").
#     Must ZAP first, then load:
sgdisk --zap-all /dev/sda
sgdisk --load-backup=/tmp/sda.gpt /dev/sda      # restores GUID 6A9387CD..., 2 parts
sgdisk -p /dev/sda                              # verify: ACCTON-DIAG 256M + DCSS 512M
cat /proc/partitions                            # sgdisk auto-BLKRRPART -> sda1/sda2 reread
                                                # (no partprobe needed/available)
# (b) partition contents
gunzip -c /tmp/sda1.img.gz | dd of=/dev/sda1 bs=1M
gunzip -c /tmp/sda2.img.gz | dd of=/dev/sda2 bs=1M
# (c) GOTCHA: no flashcp in ONIE -> write the U-Boot env via the mtdblock layer
#     (mtdblock does read-modify-erase-write; mtd2_env.bin is exactly one 64K eraseblock)
dd if=/tmp/mtd2_env.bin of=/dev/mtdblock2 bs=64k
sync
# (d) sanity: DCSS mounts and holds the ICOS images
mkdir -p /mnt/dcss; mount -t ext4 /dev/sda2 /mnt/dcss
ls -l /mnt/dcss/image1 /mnt/dcss/image2          # 30488929 bytes each
umount /mnt/dcss
```

## 5. Reboot into ICOS
- **GOTCHA — ONIE rescue has no `reboot` in PATH** (`sh: reboot: not found`). Use sysrq:
  ```sh
  sync; echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger
  ```
- U-Boot `bootcmd → nos_bootcmd` (now the restored **ICOS** version: `run bootimage1`)
  → loads `image1` from `usb:2` (DCSS) → ICOS.
- ICOS runs an **e2fsck on DCSS** ("was not cleanly unmounted, check forced") and
  auto-fixes one orphaned inode — **expected and harmless** (we just dd'd it).
- ICOS startup menu auto-selects **"1 - Start ICOS Application"** after 3 s.
- Log in `admin` / (blank) → `enable` / (blank) → `(Routing) #`.
- Confirm: `show version` → `Accton AS4610-54T, 3.4.3.7`, SN `EC2025000934`,
  `BCM56340_A0`. ✅

## 6. Return to ONL/edged afterward (VERIFIED 2026-06-06)
Reinstall the EdgeNOS/ONL **ONIE installer** (`backup/onie-installer`, 155 MB — wraps
`ONL-master_..._ARMHF.swi`) and re-push edged. Get into **ONIE rescue** the same way as
§1–2 (reboot → catch U-Boot `accton_as4610-54->` → `run onie_rescue`; re-add pubkey,
`scp -O` the installer to `/tmp`).

- **GOTCHA — run `onie-nos-install` from the SERIAL CONSOLE, not over SSH.** Over SSH the
  installer body runs on the console (you only see `line 454: reboot: not found` +
  `Failure: Unable to install image` in the SSH session, and the disk is **not** touched).
  From the serial console it runs fully and **works as-is** (no need to symlink `reboot`):
  ```sh
  # on the ONIE serial console:
  onie-nos-install /tmp/onie-installer
  ```
  It extracts the loader/initrd, repartitions to the ONL 4-partition layout
  (ONL-BOOT/CONFIG/IMAGES/DATA), unpacks the SWI, regenerates PKI, and **reboots itself
  into ONL** (the console-env `reboot` is present). ~3–4 min total.
- After ONL boots, recover mgmt over serial (the SWI reverts ma1 IP + sshd each boot):
  ```sh
  ip addr add 10.1.1.209/24 dev ma1; ip link set ma1 up
  grep -q '^PermitRootLogin yes' /etc/ssh/sshd_config || echo 'PermitRootLogin yes' >> /etc/ssh/sshd_config
  grep -q '^PasswordAuthentication yes' /etc/ssh/sshd_config || echo 'PasswordAuthentication yes' >> /etc/ssh/sshd_config
  /etc/init.d/ssh restart        # login root/onl
  ```
- Re-push the daemon (tmpfs, wiped on reboot) and bring the data plane up:
  ```sh
  scp nos/datapath/mdk-app/edged root@10.1.1.209:/tmp/edged   # then: /tmp/edged
  ```
  Expect: 48 copper bound bcm54282, L2 55/55, inventory 4×10G, 84758 ucode `0x600d`,
  `data plane UP`. (The `cpld write 0x07` warning is benign — chip still comes up fully.)

## Reboot-recovery quick card (applies to ONL boots in general)
Every ONL/EdgeNOS reboot on this box reverts two things (no persistent config):
1. **`ma1` has no IP** — ONL defaults to DHCP; if no lease, re-add static over serial.
2. **`sshd_config` resets** — `PermitRootLogin`/`PasswordAuthentication` gone; re-append + restart ssh.
Plus **`/tmp/edged` is wiped** (tmpfs) — re-scp it. Serial console `/dev/ttyUSB1`
@115200 is an always-available root shell (ONL) for recovery.
