# ASRock Rack TRX40D8-2N2T

This layer is an initial OpenBMC port for the AST2500 BMC on the ASRock Rack
TRX40D8-2N2T.

The vendor 1.30 image confirms:

- AST2500, 512 MiB DRAM with ECC enabled
- 64 MiB SPI NOR flash
- UART5 (`ttyS4`) at 115200 baud for the BMC console
- KCS channel 3 at `0xca2`
- one dedicated RGMII MAC and one RMII/NCSI MAC
- 12 I2C controllers
- the same ADC order, fan topology, KCS address, video, and USB building
  blocks as the upstream X570D4U device tree

Live UART discovery found board-specific I2C differences, so the layer now
ships a dedicated device tree. The 24C128 board EEPROM is at address `0x57`
on I2C1; X570D4U places it on I2C7 and instead declares a temperature sensor
on I2C1. The same bus exposes the NCT6796D monitoring interface at `0x2d`.

The motherboard's `TR1` header is a 3-pin thermal-sensor input. ASRock's
manual describes it as the system-TR temperature source, and the original
BMC firmware can use it for fan curves. A transient live probe with the
NCT6796 profile exposed `TSI0_TEMP` and `TSI1_TEMP` through hwmon, proving the
transport and driver path. Which TSI channel is wired to TR1 still needs a
controlled hardware correlation, so neither channel is named `TR1` yet.

The DIMM lighting controllers are also on a BMC-routed management bus. Vendor
firmware's IPMI Master Write-Read handler maps its bus 7 directly to the
AST2500 controller at `0x1e78a300`; behind it are a PCA9546-compatible mux at
`0x71` and two ENE lighting banks at `0x77`. Vendor firmware's
`ASRR_QuickSwithcTask` drives AST GPIO J3 (GPIO 75) high to route that bus to
the BMC. `ram-rgb-off.service` asserts the same quick switch only while it
programs mux channels `0x01` and `0x02`, verifies the ENE signature and Off
mode readback, restores the original mux selection, and releases the route
again. The monitor re-arms on each host-state transition, so this is entirely
BMC-native and has no host OS dependency.

The BIOS AMI inventory and configuration protocols are received over the
dedicated RNDIS Redfish host interface. Hardware is published as standard
OpenBMC CPU, DIMM, and PCI inventory. The firmware's 122-entry BIOS attribute
registry populates `bios-settings-mgr`; standard Redfish and the AMI-compatible
setup page share its pending-settings table. Neither path requires a host OS
agent. See
[`docs/ami-inventory.md`](docs/ami-inventory.md) for the protocol boundary,
BMC-local persistence, source-restricted authentication, validation results,
and the separate GPIO 219 SMI-mailbox lead.

The static update image uses 1 MiB XZ-compressed SquashFS blocks with the ARM
branch filter. The FIT already embeds the kernel as a self-decompressing
`zImage` and the initramfs as `cpio.xz`; wrapping either in another compression
layer would only add bootloader complexity. Network Virtual Media packages only
the `nbdkit` file and curl plugins that its source invokes. These choices keep
the signed update tar below bmcweb's upstream 30 MiB request limit without a
board-specific upload-size override.

## Cooling state

The board starts `phosphor-pid-control` independently of host power state and
drives all six PWM channels from the TR1 water-temperature curve. A missing or
unhealthy TR1 input selects the 100% zone fail-safe. The populated cooling
headers have been validated live at both full duty and a 30% curve target:

- FAN1: top radiator fan group; tach 1 is populated
- FAN2: water pump; no tach is reported by the vendor firmware
- FAN3: bottom radiator fan group; tach 1 is populated

The currently empty FAN4 through FAN6 tach channels are marked
missing-acceptable so that they do not create a fail-safe log storm. FAN1 and
FAN3 retain tach-based failure protection. Detailed PWM, RPM, temperature, and
service-lifecycle evidence is in `docs/live-evidence.md`.

Entity Manager stores WebUI curve edits in the writable flash overlay. The
board's versioned `trx40d8-config-migration.service` refreshes that persistent
copy when a firmware image changes the configuration schema, while preserving
and restoring the user curve before fan control starts. Increment
`config-schema-version` whenever the packaged board JSON gains fields that an
older persistent copy would mask.

Do not flash this build until the stock 64 MiB flash has been read twice with
an external programmer, both reads match, UART is connected, and a recovery
write has been rehearsed.

Host-BIOS SPI ownership, duplicate backup, and restore checks are documented
separately in [`docs/host-bios-recovery.md`](docs/host-bios-recovery.md).
The reproducible AGESA donor gate is under `tools/host-bios/`; it does not
construct or publish vendor-derived firmware.

## Build

```sh
. setup trx40d8-2n2t build/trx40d8-2n2t
bitbake obmc-phosphor-image
```

## Bring-up order

1. Validate the image and FIT contents off-target.
2. Boot from a reversible/external SPI setup if the board permits it.
3. Confirm UART, DRAM, flash partitions, MAC addresses, and both interfaces.
4. Confirm all cooling outputs begin at 100% and then follow the configured
   TR1 curve without losing the temperature input.
5. Confirm power-good and button GPIO polarity using read-only observations.
6. Test host power/reset, KCS, SOL/POST, USB virtual media, and video/KVM.
7. Revalidate the TR1 curve, pump behavior, and fail-safe path after changes to
   the fan topology or sensor configuration.

Unknown or unverified devices are documented in `docs/bringup.md`.
