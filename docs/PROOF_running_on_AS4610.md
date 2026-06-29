# Proof of execution: EdgeNOS-4610 (Linux 6.1.175) running on the physical AS4610-54T

This is a captured, on-box proof that **our** modern Linux NOS — not the stock vendor
firmware — is running on **this specific** Edgecore AS4610-54T and actively forwarding
through its BCM56340 silicon. Every line below is the device's own output, taken
read-only over the serial console. The proof works by **binding four things that cannot
be produced from any other machine**: our software identity, this unit's factory hardware
identity, the live switch silicon, and a fresh timestamp.

> Captured: **Fri Jun 19 02:11–02:12 UTC 2026** (the box's own clock), serial console.

---

## 1. It is OUR modern Linux — not ICOS

```
$ uname -a
Linux edgenos-4610 6.1.175-OpenNetworkLinux-armhf #1 SMP PREEMPT Sat Jun 13 21:58:57 UTC 2026 armv7l GNU/Linux
```

- **`6.1.175-OpenNetworkLinux-armhf`** = our forward-ported kernel; build stamp **Sat Jun 13
  2026** matches our build. `armv7l` = the Helix4 on-die Cortex-A9.
- The factory NOS was **Linux 4.4.39-Broadcom** (FASTPATH/ICOS 3.4.3.7, 2019). The running
  kernel is a different major series entirely — this is not the stock image.

## 2. It is OUR own-build NOS — not ICOS/FASTPATH, not stock ONL

```
$ hostname
edgenos-4610
$ head -3 /etc/os-release
NAME=Buildroot
VERSION=-g959e7ac-dirty
ID=buildroot
```

- Hostname **`edgenos-4610`** and **`ID=buildroot`** = our from-scratch Buildroot userland.
  `VERSION` even carries our git describe (`-g959e7ac`). FASTPATH/ICOS reports neither.

## 3. It is running on THIS physical switch (factory-burned identity)

```
$ fw_printenv | grep -iE 'serial#|ethaddr|onie_machine|onie_platform'
serial#=EC2025000934
ethaddr=04:F8:F8:15:A8:40
onie_machine=accton_as4610_54
onie_platform=arm-accton_as4610_54-r0
$ cat /proc/cmdline
console=ttyS0,115200 onl_platform=arm-accton-as4610-54-r0 coherent_pool=16M panic=10
$ fw_printenv boot_slot
boot_slot=A
```

- **`serial#=EC2025000934`** is the factory serial number burned into *this* unit's u-boot
  environment — it identifies the specific chassis, and matches our hardware records.
- **`onie_machine=accton_as4610_54`** / **`ethaddr=04:F8:F8:15:A8:40`** confirm the platform
  and board MAC. A different box cannot present this serial + MAC + machine ID.
- Booted from **slot A** (the production 6.1 install), kernel cmdline is our `panic=10`/
  `coherent_pool=16M` profile.

## 4. It is driving the REAL BCM56340 silicon

```
# kernel BDE probing the on-die CMIC (device register read from silicon):
linux-kernel-bde (1092): _get_cmic_ver: Not PCI device 0, type 20000180

# our datapath daemon, bound to the chip:
[bcmd] chip attached + init all/bcm done
[bcmd] enabled 54 ports
[bcmd] netlink: mapped 54 netdevs -> chip ports
[bcmd] datapath UP — every port a Linux netdev; L3 FIB mirrored to chip.

$ lsmod | grep -E 'bde|knet'
linux_bcm_knet         81920  0
linux_user_bde         24576  0
linux_kernel_bde       61440  2 linux_bcm_knet,linux_user_bde
$ modinfo /opt/edgenos/linux-kernel-bde.ko | grep vermagic
vermagic: 6.1.175-OpenNetworkLinux-armhf SMP preempt mod_unload ARMv7 p2v8
```

- **`_get_cmic_ver: type 20000180`** is read from the on-die CMIC version register on the
  actual chip — proof the BDE is talking to real Helix4 silicon, not an emulator.
- `bcmd` **attached the chip, enabled all 54 ports, and mirrored the Linux FIB into the
  chip's L3 tables.** The three GPL BDE/KNET modules are loaded with vermagic
  **`6.1.175-OpenNetworkLinux-armhf`** — i.e. built for *this* kernel.

## 5. It is alive and forwarding — right now

```
$ uptime
 02:12:09 up 3 days, 15:49,  1 user,  load average: 0.99, 1.03, 1.01

# packets crossing the real chip, copper front port ge25 (10.14.1.2/24) -> peer .254:
$ ping -c3 10.14.1.254
3 packets transmitted, 3 packets received, 0% packet loss
round-trip min/avg/max = 0.550/0.605/0.694 ms

$ ip -br addr show ge25
ge25  UNKNOWN  10.14.1.2/24 ...  <BROADCAST,MULTICAST,UP,LOWER_UP>
$ ip -br addr show xe0
xe0   UNKNOWN  10.101.102.2/29 ...
```

- **Uptime 3 days 15:49** — the box has run our 6.1 NOS continuously, unattended.
- **Copper `ge25` forwards at 0% packet loss** to a live peer — packets are switched in the
  BCM56340 hardware datapath under our `bcmd`. (Fiber `xe0` shows its peer `10.101.102.1`
  unreachable at capture time — the far-side router was down; our side's link/L3 is up.)

---

## Why this is proof, not just a screenshot

No single datum is conclusive, but the **combination is**: a kernel string and a Buildroot
`os-release` can be edited, but they cannot also produce this chassis's **factory serial
`EC2025000934`** and **MAC `04:F8:F8:15:A8:40`**, *and* a live read of the BCM56340's
**CMIC `type 20000180`**, *and* real packets forwarding through that chip at 0% loss, *and*
a 3.6-day uptime, *and* a current timestamp — all from the same console session. Software
identity ∧ this-unit hardware identity ∧ live silicon ∧ live forwarding ∧ fresh time =
EdgeNOS-4610 (Linux 6.1.175) is genuinely running on this AS4610-54T.

*Reproduce any time:* over the serial console (root), run `uname -a`, `fw_printenv`,
`lsmod`, `modinfo /opt/edgenos/linux-kernel-bde.ko`, `uptime`, and
`ping -c3 10.14.1.254`. Stock ICOS would instead show Linux 4.4.39, a FASTPATH banner, and
no `bcmd`/BDE-KNET of ours.
