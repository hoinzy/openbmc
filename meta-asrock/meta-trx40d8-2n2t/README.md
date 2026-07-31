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
`0x71` and two ENE lighting banks at `0x77`. `ram-rgb-off.service` monitors
that bus and applies ENE Off mode to mux channels `0x01` and `0x02` whenever
the BMC owns the route. It verifies the ENE signature and mode readback and
restores the original mux selection after every attempt. This replaces the
earlier host-side IPMI workaround and requires no host OS service.

The BIOS AMI inventory and configuration protocols are received over the
dedicated RNDIS Redfish host interface. Hardware is published as standard
OpenBMC CPU, DIMM, and PCI inventory. The firmware's 122-entry BIOS attribute
registry populates `bios-settings-mgr`; standard Redfish and the AMI-compatible
setup page share its pending-settings table. Neither path requires a host OS
agent. See
[`docs/ami-inventory.md`](docs/ami-inventory.md) for the protocol boundary,
BMC-local persistence, source-restricted authentication, validation results,
and the separate GPIO 219 SMI-mailbox lead.

## Safety state

The first implementation inherits the known X570D4U wiring and overrides only
facts confirmed on the TRX40 board. Cooling remains open-loop. No PID fan
controller is installed. The firmware must start all three populated cooling
headers at full PWM duty:

- FAN1: top radiator fan group; tach 1 is populated
- FAN2: water pump; no tach is reported by the vendor firmware
- FAN3: bottom radiator fan group; tach 1 is populated

Do not flash this build until the stock 64 MiB flash has been read twice with
an external programmer, both reads match, UART is connected, and a recovery
write has been rehearsed.

## Build

```sh
. setup trx40d8-2n2t build/trx40d8-2n2t
bitbake obmc-phosphor-image
```

## Bring-up order

1. Validate the image and FIT contents off-target.
2. Boot from a reversible/external SPI setup if the board permits it.
3. Confirm UART, DRAM, flash partitions, MAC addresses, and both interfaces.
4. Confirm all cooling outputs are at 100% before powering the host.
5. Confirm power-good and button GPIO polarity using read-only observations.
6. Test host power/reset, KCS, SOL/POST, USB virtual media, and video/KVM.
7. Add closed-loop fan control only after pump behavior and every thermal
   sensor have been verified.

Unknown or unverified devices are documented in `docs/bringup.md`.
