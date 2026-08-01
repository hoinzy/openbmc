# Host BIOS backup and recovery

The TRX40D8-2N2T host firmware is a 16 MiB SPI flash connected to the AST2500
SPI1 controller. The vendor BMC's `host_spi_flash_hw` module selects the SPI1
pinmux in SCU registers `0x70` and `0x7c`. Reverse engineering of the vendor
`libflash.so.6.18.0` also found the board-specific ownership mux: its
`SwitchExternalGPIO(1)` path drives legacy GPIO 73 (AST2500 GPIOJ1) low before
the host-SPI driver is loaded. The release path drives GPIOJ1 high for 100 ms
and then changes it back to input.

OpenBMC marks SPI1 for manual binding. During normal operation the Aspeed SPI
driver is not attached, so its pinctrl state is released to the host. The
vendor module also enables both SPI1 chip-select paths and sanitizes the CE0
control register before flash detection. OpenBMC reproduces that initialization
only after an explicit, host-off manual bind. The device tree names GPIOJ1
`output-bios-flash-bmc-select-n`, and the updater follows this sequence:

1. Require `CurrentHostState=Off`.
2. Drive GPIOJ1 low and hold the line for the complete operation.
3. Set the platform-device `driver_override` to `spi-aspeed-smc`.
4. Bind `1e630000.spi`, locate the `bios` MTD device, and read or write it.
5. Unbind the driver and clear the override on success, failure, or signal.
6. Drive GPIOJ1 high for 100 ms and release it to input.

An image without the vendor-compatible controller initialization failed safely
with JEDEC bytes `00 00 00 00 00 00`: no MTD device or backup file was created,
and the cleanup path restored the unbound idle state. That result is the reason
the `aspeed,host-flash-init` quirk is board-gated rather than applied to every
ASPEED SPI controller. A subsequent reversible live test with GPIOJ1 asserted
low discovered the `bios` MTD immediately; unbind plus the vendor-compatible
release sequence returned the line high and unclaimed.

## First-deployment checks

Apply the first BMC image containing this support only while the host is off.
Before powering on the host, verify that SPI1 is idle:

```sh
test ! -L /sys/bus/platform/drivers/spi-aspeed-smc/1e630000.spi
! grep -qx bios /sys/class/mtd/*/name 2>/dev/null
cat /sys/bus/platform/devices/1e630000.spi/driver_override
gpiofind output-bios-flash-bmc-select-n
gpioinfo gpiochip0 | grep output-bios-flash-bmc-select-n
gpioget $(gpiofind output-bios-flash-bmc-select-n)
```

The first two commands must succeed. `driver_override` must be empty or print
`(null)`. GPIOJ1 must be unused, configured as input, and read high.

## Capture duplicate backups

With the host still off:

```sh
bios-update.sh -r /tmp/trx40d8-host-bios-a.bin
bios-update.sh -r /tmp/trx40d8-host-bios-b.bin
sha256sum /tmp/trx40d8-host-bios-*.bin
cmp /tmp/trx40d8-host-bios-a.bin /tmp/trx40d8-host-bios-b.bin
```

Both reads must be 16 MiB, contain `aa55aa55` at offset `0x20000`, and compare
identically. Copy both files off the BMC before any write. A runtime backup may
contain NVRAM state and therefore is not required to hash identically to the
vendor update file.

After each read, repeat the idle checks above and only then power on the host.
Confirm that POST, KVM video, SOL, the booted OS, sensors, and fan control are
still functional.

## Restore the captured stock image

Only with the host off, copy one verified backup to the BMC and run:

```sh
bios-update.sh /tmp/trx40d8-host-bios-a.bin
```

Do not interrupt BMC power during `flashcp`. Verify that SPI1 is idle before
powering on the host. Keep the external programmer and original BMC recovery
chip available until this path has completed a real read/write/readback
rehearsal.

## AGESA experiment gate

The current reverse-engineering result does not permit a custom image. AGESA
1.0.0.F is coupled to hundreds of UEFI modules and a newer signed PSP stack,
while APCB differs on every donor board. The ASRock APCB must be preserved, but
its compatibility with that module set has not been established. Use
`tools/host-bios/analyze.py` to reproduce the comparison; it intentionally
returns status 2 and emits no ROM while the gate is closed.
