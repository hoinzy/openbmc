# Initial assessment and effort estimate

## Machine

| Component | Observed value |
| --- | --- |
| Mainboard | ASRock Rack TRX40D8-2N2T |
| Host CPU | AMD Ryzen Threadripper 3970X |
| Host memory | 128 GiB |
| Host OS | Ubuntu 24.04 |
| BIOS | L1.19F |
| BMC | AST2500, vendor firmware 1.30 |
| BMC IPMI ID | manufacturer 49622, product `0x0202` |
| Host KCS | channel 3 at `0xca2` |
| BMC flash | 64 MiB SPI NOR |
| BMC RAM | 512 MiB, ECC enabled |

The host exposes the AST1150 PCI bridge and the AMI virtual USB hub, CD-ROM,
Ethernet, and HID functions. BIOS `Onboard VGA` has now been selected as the
primary display and the physical monitor disconnected. `obmc-ikvm` negotiates
a 1024x768 mode, but both its VNC framebuffer and the WebUI capture remain
black after host boot.

## Upstream starting point

OpenBMC already supports several ASRock Rack AST2500 boards. X570D4U remains
the closest structural match and supplies:

- 64 MiB flash layout and 512 MiB memory
- dedicated RGMII plus RMII/NCSI networking
- EEPROM-backed MAC addresses (the TRX40 EEPROM is on a different bus)
- KCS at `0xca2`, LPC snoop at port `0x80`, and VUART
- AST video, graphics, PCI-to-AHB, USB virtual hub, EHCI, and UHCI
- 13 ADC inputs in the exact order reported by the TRX40 vendor SDR
- six PWM outputs and the same paired-tach fan topology
- plausible ASRock power/reset/POST/power-good GPIO line names

ROMED8HM3 and the existing ASRock entity-manager configurations provide the
closest AMD sensor and regulator examples.

No upstream TRX40D8-2N2T machine, device tree, entity-manager configuration, or
board-specific test record existed when this assessment was made.

Live vendor-kernel scanning disproved a complete I2C match. X570D4U declares
`w83773g@4c` on I2C1 and its EEPROM on I2C7. The TRX40 has no `0x4c` device on
I2C1 and its physical board EEPROM is at I2C1 address `0x57`. The other I2C1
responder at `0x2d` is the NCT6796D back-door monitoring interface: a
transient `nct6796` probe exposed `TSI0_TEMP` at 36.5 C and `TSI1_TEMP` at
55.75 C. A PCA9545-like device is present at I2C4 address `0x70` on both
designs. TRX40 I2C3 contains seven responding addresses which are not yet
assigned drivers.

## Implemented baseline

- discoverable `trx40d8-2n2t` Yocto machine and build template
- AST2500/64 MiB configuration with a dedicated TRX40 device tree patch for
  Linux, inheriting X570D4U and moving the EEPROM to its verified bus
- NCT6775 I2C hwmon kernel support
- host power/reset state configuration
- KCS/SOL at `0x2f8`, SIRQ 3, 115200 baud
- vendor-compatible IPMI device identity
- entity-manager definitions for all 13 verified voltage inputs
- FAN1 and FAN3 tach sensors
- open-loop full-duty startup for every declared PWM channel

The baseline has now booted successfully from RAM on the target. The physical
FRU selected the TRX40 Entity Manager configuration, all 13 voltage sensors
instantiated, standby readings were plausible, the dedicated MAC linked at
1 Gbit/s, and every PWM control read `255`. This proves the kernel/device-tree
and inventory baseline without claiming flash installation or a full systemd
boot.

The board DTS is now carried by the tracked Linux patch in this layer rather
than being copied into the kernel work tree by a recipe task. This keeps the
kernel dependency reviewable and preserves the validated `gfx_memory` fix.

The baseline intentionally does not claim unverified temperature sensors,
chassis intrusion, closed-loop fan control, or BIOS update support. A later
powered-host pass also validated the configured power-control lifecycle and
Redfish `PowerState: On`; thermal and fan/PSU Redfish coverage remains
incomplete.

The first RAM boot exposed one inherited X570D4U device-tree bug: the reserved
graphics framebuffer was not linked to the GFX controller. The local board
override now adds the binding-required `memory-region`; a rebuilt RAM boot is
now confirmed to initialize DRM and the AST framebuffer. An end-to-end
BIOS-primary-display test reaches a valid 1024x768 capture mode but still
returns a black frame. Kernel video timings renegotiate correctly, so the
remaining issue is host display routing or framebuffer content rather than
WebUI screenshot generation.

It also showed that the inherited NCSI pinctrl transaction fails: live debugfs
shows neither RMII2 nor MDIO2 applied, including with an RMII2-only test DTB.
The cause is not yet established, and channel discovery still requires a
host-on test, so the upstream X570D4U pinctrl description remains unchanged.

## Remaining work

| Workstream | State | Focused engineering time |
| --- | --- | ---: |
| Boot, flash, DRAM, UART, dedicated MAC | RAM boot passed; external-programmer recovery and flash boot remain | 1-3 days |
| NCSI and MAC EEPROM offsets | both MACs verified at `0x3f80`/`0x3f88`; live NCSI discovery still fails and pin 24 reports `EPERM` | 1-2 days |
| Power/reset/state GPIO | powered-host pass validated GPIO ownership and power/reset/POST transitions; repeat on a clean boot and upstream the evidence | 1-2 days |
| KCS, POST snoop, SOL | definitions exist; exercise full host lifecycle | 1-3 days |
| Voltage sensors | channel map and thresholds implemented | 1-2 days target calibration |
| Temperature sensors | NCT6796D I2C path is identified and its two TSI channels are exposed by a transient probe; TR1 channel correlation and remaining sensors are still open | 2-6 days |
| Pump/fan control | full-duty baseline safe; characterize outputs before PID | 3-7 days |
| KVM/video | AST path negotiates 1024x768 with onboard VGA primary, but captures remain black | 2-5 days |
| KVM/USB stability | first S0 KVM use correlated with BMC RAM-boot reset/fallback and host USB `-71`; UART reproduction required | 2-5 days |
| Virtual media | WebSocket/NBD negotiation and AST mass-storage gadget pass in RAM after the board-layer jsnbd fix; host enumeration and sustained reads remain | 1-2 days |
| Redfish thermal/power surface | host/chassis `PowerState: On` now works; fan and PSU collections are empty, ThermalMetrics returns HTTP 500, and host CPU/DIMM/PCI inventory is absent | 2-4 days |
| LEDs, intrusion, PROCHOT/THERMTRIP | GPIO mapping and policy required | 2-5 days |
| Upstream-quality dedicated DTS and reviews | split kernel/entity-manager/openbmc changes | 1-3 weeks |

From the current RAM-booted baseline, a mostly functional lab port is
approximately **1-3 focused weeks** if the X570D4U GPIO assumptions hold. A
recovery-tested, cooling-controlled, upstream-ready port is approximately
**3-6 weeks**, dominated by hardware test windows, temperature-device
discovery, KVM/USB validation, flash recovery rehearsal, and review across
multiple upstream repositories.

## Recovery gate

Before the first target boot:

1. Read the installed SPI flash twice with an external programmer.
2. Verify both 64 MiB reads have identical SHA-256 hashes.
3. Test erasing, writing, and reading back a spare compatible flash device.
4. Connect 3.3 V UART and record the complete vendor boot.
5. Verify the board can be recovered with the host fully unpowered.
6. Keep the known-good original flash device untouched.

The supplied update image is useful for layout analysis but is not a substitute
for a programmer read of the machine's installed flash because it does not
contain the machine-specific writable state and MAC/FRU data.
