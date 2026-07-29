# TRX40D8-2N2T bring-up notes

## Evidence from vendor firmware 1.30

The update archive contains one 67,109,128-byte `.ima`. Its first 64 MiB are
the SPI flash payload; the remainder is a signed trailer. The payload contains
two redundant 2 MiB JFFS2 configuration regions, a CramFS root filesystem, a
Linux 3.14.17 AMI kernel, a web/KVM CramFS, and a small JFFS2 region.

The vendor project file records these platform constants:

| Item | Value |
| --- | --- |
| SoC | AST2500 |
| SPI flash | 64 MiB at `0x20000000` |
| erase size | 64 KiB |
| U-Boot | 256 KiB |
| U-Boot environment | `0x20040000`, 64 KiB |
| RAM | 512 MiB physical, ECC enabled |
| BMC console | UART5 / `ttyS4`, 115200 |
| I2C controllers | 12 |
| MAC EEPROM | bus 1, address `0x57` |
| KCS | channel 3, host I/O `0xca2` |

The physical EEPROM begins with a valid IPMI FRU and identifies manufacturer
`ASRockRack` and product `TRX40D8-2N2T`. Its MAC region contains:

| Offset | Purpose | Verified value |
| ---: | --- | --- |
| `0x3f80` | dedicated MAC | `d0:50:99:f4:22:1e` |
| `0x3f88` | NCSI MAC | `d0:50:99:f4:20:d0` |

The `FRU.bin` file inside both vendor configuration partitions is unrelated
template data for an Intel S2600WFT. It must not be used as board identity or
written over the physical EEPROM.

The live vendor SDR exposes ADC channels in the same order as the X570D4U
device tree:

| ADC | Sensor |
| ---: | --- |
| 0 | 3VSB |
| 1 | 5VSB |
| 2 | VCPU |
| 3 | VSOC |
| 4 | VCCM_AB |
| 5 | VCCM_CD |
| 6 | PM_VDD_CLDO |
| 7 | PM_VDDCR_S5 |
| 8 | PM_VDDCR |
| 9 | BAT |
| 10 | 3V |
| 11 | 5V |
| 12 | 12V |

## Live I2C inventory

The vendor driver numbers adapters `0` through `11`. Its scanner prints
left-shifted 8-bit addresses; the normalized 7-bit results are:

| Bus | Responding 7-bit addresses |
| ---: | --- |
| 0 | none |
| 1 | `0x57` (24C128 board FRU/MAC EEPROM) |
| 2 | none |
| 3 | `0x13`, `0x14`, `0x15`, `0x54`, `0x55`, `0x69`, `0x73` |
| 4 | `0x70` (four-channel mux topology shared with X570D4U) |
| 5-8 | none while the host was off |
| 9-11 | controller reported busy; no recovery or forced transactions attempted |

Address presence does not establish device type. In particular, no child
devices are declared yet for I2C3.

## Confirmed and inherited wiring

- The dedicated TRX40 device tree inherits X570D4U for the matching ADC order,
  six PWM channels, paired fan tachs, NCSI choice, KCS address, UART, video,
  USB, and currently assumed GPIO topology.
- It removes X570D4U's unobserved `w83773g@4c`, removes its I2C7 EEPROM node,
  and declares the verified 24C128 on I2C1.
- The X570D4U GPIO line names and power-control polarity still require
  observation on this board before host-control tests.
- The FRU product probe is exact: manufacturer `ASRockRack`, product
  `TRX40D8-2N2T`.

## Still to discover

- Device identities and sensor mapping for the seven I2C3 responders, including
  CPU, motherboard, card-side, TR1, and eight DIMM temperature sources.
- The board manual identifies TR1 as a 3-pin thermal-sensor header and lists
  system-TR temperature sensing in the hardware monitor. The live BMC image
  has no TR1 hwmon channel, no identified temperature child on the scanned BMC
  buses, and no `0x4c` device matching the inherited X570D4U temperature node.
  Treat TR1 as a host/Super-I/O or vendor-IPMI inventory path until that path
  is traced; do not add a guessed BMC DTS node.
- Whether CPU temperature uses SB-TSI, PECI compatibility logic, or an
  external monitor.
- The exact `BMC_READY`, PROCHOT, THERMTRIP, chassis-intrusion, POST-complete,
  sleep-state, and power-good GPIO assignments.
- The BMC video path with BIOS `Onboard VGA` selected as primary.
- Virtual media behavior through the AST2500 virtual USB hub.

## Host inventory and Redfish collections

The BMC kernel has no `/sys/bus/pci/devices` hierarchy. Consequently, the
Redfish `PCIeDevices` collection is empty, and the current system resource
reports zero processors and zero system memory. These are host-inventory
publication gaps, not evidence that the AST2500 PCI-to-AHB support is absent.
The board layer currently provides Entity Manager inventory for the BMC board,
voltages, and fan tachometers only. Publishing host CPU, DIMM, PCI device, and
PCIe-slot objects requires a host inventory provider and board-specific
correlation; static slot names must not be substituted for discovered devices.

## Cooling invariant

Until all thermal sensors and fan outputs are verified, FAN1, FAN2, and FAN3
must remain at full duty and no automatic fan controller may be enabled. FAN2
is the water pump and must never be slowed or stopped. The inherited AST2500
`aspeed-pwm-tacho` driver initializes every declared PWM port to `0xff`;
target bring-up must still verify the physical polarity and resulting speed.

## Reversible first boot

Do not make the first boot by replacing the installed SPI contents. Target
testing confirmed that the 2020 vendor U-Boot rejects OpenBMC FIT images with
`Unknown image format` and was built without command-line FDT handling. The
kernel, initramfs, and DTB can nevertheless be booted entirely from RAM with a
legacy kernel header and a transient change to U-Boot's in-RAM board data.

Create the legacy bundle from the built FIT:

```sh
tools/make-legacy-ram-boot.sh \
    path/to/fitImage \
    trx40d8-2n2t-ramboot.bin
```

The helper prints all source addresses and sizes used below. Place the bundle
in a TFTP root on a directly reachable server. Send UART commands one at a
time; the vendor console drops characters when commands are sent as a burst.

The following board-data address is verified only for ASRock Rack vendor
U-Boot `2013.07` built `2020-09-16`. Confirm the adjacent values before
changing it:

```text
md.l 9b6baff4 4
```

The first three words must be the machine ID `00000384`, boot-parameter
pointer `80000100`, and DRAM start `80000000`. Redirect only the boot-parameter
pointer to a safe DTB address and verify it:

```text
mw.l 9b6baff8 82000000 1
bdinfo
```

Configure the dedicated interface, transfer the bundle, and require a valid
legacy-image CRC:

```text
setenv ethact ast_eth0
setenv ethrotate no
setenv ipaddr 192.168.178.52
setenv serverip 192.168.178.41
setenv netmask 255.255.255.0
ping 192.168.178.41
tftpboot 83000000 trx40d8-2n2t-ramboot.bin
iminfo 83000000
```

For the verified `20260726112611` build, the tail components and CRCs are:

```text
crc32 83319000 12ec48
# 5e459523
crc32 83448000 7592
# 5ea20009
```

Copy the initramfs, prepare the legacy kernel, overwrite the generated ATAGs
with the verified DTB, and jump:

```text
cp.b 83319000 84000000 12ec48
crc32 84000000 12ec48
setenv bootargs 'console=ttyS4,115200 earlycon rdinit=/bin/sh'
bootm start 83000000
bootm loados
bootm prep
cp.b 83448000 82000000 7592
crc32 82000000 7592
bootm go
```

Do not run `saveenv`. None of these commands writes SPI flash. Resetting the
BMC restores the original board-data pointer and vendor boot path.

At the initramfs shell, mount the virtual filesystems and collect the
non-destructive baseline before attempting a flash-backed boot:

```sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
cat /proc/cmdline
cat /proc/mtd
cat /proc/iomem
dmesg
```

The test is successful only if UART remains usable, the full 512 MiB physical
DRAM geometry is plausible, the MX66L51235F is detected as 64 MiB, the
dedicated MAC is read from EEPROM, and all populated cooling outputs can be
shown to remain at full duty. The verified RAM boot reached Linux 6.18 and a
root shell, with all six PWM controls reading `255`. Tach inputs timed out in
S5 and still require a host-power test. Resetting the BMC returns to the
untouched vendor firmware.

The framebuffer phandle fix was also validated without a Yocto rebuild by
patching the RAM-only DTB and transferring
`trx40d8-2n2t-ramboot-fix2.bin`. Its bundle is `0x44f5ae` bytes; the corrected
DTB is `0x75ae` bytes with CRC32 `59de1794`. The kernel reported:

```text
aspeed_gfx 1e6e6000.display: assigned reserved memory node framebuffer
[drm] Initialized aspeed-gfx-drm 1.0.0
aspeed_gfx ... fb0: aspeed-gfx-drmd frame buffer device
```

This remains a RAM test artifact; the source DTS change must be included in a
fresh Yocto build before any flash decision.

### Virtual-media validation

The Web UI's local-image path uses the bmcweb `/vm/0/0` WebSocket and the
`jsnbd` NBD proxy. On this board, the OpenEmbedded `nbd-client` rejects the
legacy `-L` argument used by the pinned jsnbd revision. The machine layer
therefore applies a small patch that removes only that argument for the Unix
socket connection. Without it, `nbd-proxy` exits immediately and no USB
mass-storage gadget is created.

With the patch applied, the safe RAM test is:

1. Log into the HTTPS Web UI and open Operations → Virtual Media.
2. Select a small test image and start the device.
3. Confirm `/dev/nbd0` has a non-zero size and that
   `/sys/kernel/config/usb_gadget/mass-storage` is bound to one AST virtual-hub
   UDC (`1e6a0000.usb-vhub:p1` on the initial test).
4. With the host powered and the BMC USB path connected, verify that the host
   enumerates the mass-storage device and can read the image.
5. Stop the Web UI session and confirm the gadget and `/dev/nbd0` are released.

The first three checks passed in RAM. Host-side enumeration and sustained data
transfer remain an open test item; do not treat gadget creation alone as proof
that boot-from-virtual-media works.

## Flash installation gate

The OpenBMC `static.mtd` image is a complete 64 MiB replacement. It installs a
different U-Boot and partition table and is not an AMI `.ima` update payload.
Never send it to the vendor web updater or `fwupdate`.

Before installing it:

1. Fully remove AC and standby power before attaching a programmer; do not
   back-power the board through the SPI header or clip.
2. Use a programmer that explicitly supports the 3.3 V, 512-Mbit
   MX66L51235F and 4-byte addressing.
3. Read the installed chip twice into separate 64 MiB files and require
   identical SHA-256 hashes.
4. Program and verify a spare compatible chip with the OpenBMC image while
   preserving the untouched original chip as the recovery image.
5. Reinstall/connect only the verified spare, attach UART, and initially keep
   host ATX power disabled.
6. Confirm U-Boot, kernel, flash partitions, UART, MAC addresses, and 100%
   cooling duty before allowing the host to power on.

With `flashrom`, the programmer-side sequence is conceptually:

```sh
flashrom -p PROGRAMMER -r stock-a.bin
flashrom -p PROGRAMMER -r stock-b.bin
sha256sum stock-a.bin stock-b.bin
cmp stock-a.bin stock-b.bin
flashrom -p PROGRAMMER -w obmc-phosphor-image-trx40d8-2n2t.static.mtd
```

Replace `PROGRAMMER` only after detecting the exact programmer and flash chip.
The normal `flashrom -w` path verifies the write. Do not use these commands
in-system and do not proceed if either read is not exactly 67,108,864 bytes.
