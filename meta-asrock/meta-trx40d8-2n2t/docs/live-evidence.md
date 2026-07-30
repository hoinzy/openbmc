# Live evidence record

This file records target observations used by the initial port. All invasive
changes were made only in RAM and disappear on BMC reset.

## Vendor firmware

- BMC firmware: ASRock Rack/AMI `1.30.00`
- U-Boot: `2013.07`, built 2020-09-16
- Linux: vendor 3.14.17, UART5 (`ttyS4`) at 115200
- SPI NOR: Macronix MX66L51235F, 64 MiB
- root filesystem: read-only CramFS at flash offset `0x00460000`
- usable RAM: 440 MiB after ECC/reserved regions

## Captured configuration

The live read-only JFFS2 configuration mount and both extracted firmware banks
contained identical files:

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| `FRU.bin` vendor template | 224 | `c16e03f810890c82592e2b8183faf28e26b9a404ec8efed7d74bd0d5edb6d809` |
| `SDR.dat` | 3840 | `91c6798f3db8bf5633a78b8a0e2f381d1577d599f2691855117a600282702eee` |

`SDR.dat` contains 65 records: 13 board voltages, four board temperatures,
eight DIMM temperatures, two PSU temperatures, 12 fan tachometers, PSU
telemetry/status, CPU PROCHOT/THERMTRIP, chassis intrusion, eight ECC event
records, PEF, BMC FRU, and AST2500 records.

## EEPROM integrity incident

During early discovery, an incorrectly ordered invocation of the vendor
`i2c-test` utility interpreted two option arguments as EEPROM payload and
changed bytes `0x3f80..0x3f81` from `d0 50` to `00 06`. The change was detected
immediately, the two original bytes were restored, and the complete 16-byte
MAC region was read back as:

```text
d0 50 99 f4 22 1e 13 ff d0 50 99 f4 20 d0 63 ff
```

Subsequent EEPROM reads use combined repeated-start mode with the read count
specified before the final two-byte EEPROM offset. No further writes are used
for discovery.

## OpenBMC RAM boot

The `20260726112611` build was booted entirely from RAM through the vendor
U-Boot and a Synology TFTP server. No flash command, `saveenv`, MTD write, or
host-power operation was used.

The vendor U-Boot rejected `fitImage` with `Unknown image format`. A generated
legacy bundle was transferred to `0x83000000`; U-Boot verified its kernel CRC.
The vendor board-data boot-parameter pointer was changed transiently from
`0x80000100` to `0x82000000`, avoiding the ARM decompressor workspace. Kernel,
initramfs, and DTB CRC32 values in target RAM were:

| Component | Address | Size | CRC32 |
| --- | ---: | ---: | --- |
| kernel | `0x81000000` | `0x318de0` | `0bf40ade` |
| initramfs | `0x84000000` | `0x12ec48` | `5e459523` |
| DTB | `0x82000000` | `0x7592` | `5ea20009` |

Linux `6.18.39-adcf448-patch-c5a4df5` reached a root shell and reported model
`ASRock Rack TRX40D8-2N2T BMC`. Live enumeration confirmed:

- 512 MiB physical memory and the 64 MiB MX66L51235F;
- KCS channel 3 at `0xca2`, VUART `ttyS5`, and console `ttyS4`;
- board EEPROM at I2C1 `0x57`, with both expected MAC addresses;
- PCA9545 at I2C4 `0x70`, creating buses 40 through 43;
- AST video as `/dev/video0`;
- dedicated RTL8211E Ethernet at 1 Gbit/s and the NCSI MAC;
- six PWM controls, all initialized to `255`;
- 13 ADC inputs and nine exported tach inputs.

All tach reads timed out with the host in S5. The two configured fan sensors
therefore reported `NaN`; physical cooling behavior still requires a guarded
host-power test.

The OpenBMC squashfs was downloaded separately, SHA-256 verified, padded only
in RAM to a block boundary, and loop-mounted read-only. An isolated in-RAM
journald/D-Bus/ObjectMapper/FruDevice/EntityManager stack then:

- parsed the physical FRU as manufacturer `ASRockRack`, product
  `TRX40D8-2N2T`, serial `202252540000041`;
- selected and posted the TRX40 Entity Manager configuration;
- instantiated all 13 configured voltage objects and FAN1/FAN3;
- reported live standby rails `3VSB = 3.33 V`, `5VSB = 5.0578 V`, and
  `VBAT = 2.8683 V`; host-switched rails correctly reported unavailable with
  the host in S5.

The read-only root filesystem also contains the enabled service definitions
and executables for bmcweb, KVM, console/SOL, host and network IPMI, KCS
bridging, and x86 power control. They were not started in this isolated test:
doing so without a normal PID 1 dependency graph would not validate their
real lifecycle, and power control was intentionally excluded.

Expected limitations of the minimal initramfs test were USB mass-storage
function errors with no LUN, missing chassis-state providers for sensor
`PowerState` queries, and no full systemd boot. The AST graphics
reserved-memory error was traced to the inherited X570D4U DTS reserving
`gfx_memory` without assigning it to `&gfx`; the board override now supplies
the binding-required `memory-region` phandle. The corrected RAM-only boot
confirmed DRM probe and `/dev/dri/card0` plus `/dev/fb0` after devtmpfs was
mounted. The NCSI MAC still reports an initial pinctrl error. Live pinctrl
debugfs showed
that neither RMII2 nor MDIO2 is applied, including when tested with an RMII2-only
DTB override. The cause is not yet established; a rebuilt boot with the host
powered must validate the board's NCSI wiring and traffic before changing the
upstream X570D4U pinctrl description.

## Full systemd and virtual-media evidence

The corrected OpenBMC root filesystem was then started with the normal systemd
userspace from RAM. `bmcweb`, KVM, host IPMI, network IPMI, the KCS bridge,
Entity Manager, and the network manager reached an active state. Redfish was
available over HTTPS and the Web UI session-cookie flow authenticated the
virtual-media WebSocket at `/vm/0/0`.

The first virtual-media attempt exposed a dependency mismatch rather than a
board wiring failure: the image's `nbd-proxy` passed `-L` to `nbd-client`, but
the OpenEmbedded nbd-client rejected that option for the Unix-socket export and
exited before creating the gadget. Removing that argument in the RAM overlay
allowed the complete negotiation to run:

- `/dev/nbd0` was configured as a 1 MiB export (`2048` sectors);
- `/sys/kernel/config/usb_gadget/mass-storage` was created;
- the gadget was bound to `1e6a0000.usb-vhub:p1`;
- the backing LUN was `/dev/nbd0`.

The test gadget and NBD connection were detached afterward. The board layer
now carries the same `jsnbd` change for the next image build. Physical host
USB enumeration and sustained image reads remain unverified because the host
was not powered for this test. The initial initramfs `g_mass_storage` errors
are expected there because no LUN is supplied until a WebSocket session starts.

## Current full-userspace deficiencies

- The initial S5 boot failed
  `xyz.openbmc_project.Chassis.Control.Power@0.service` while requesting the
  inherited `button-power-n` line (`Operation not permitted`). A subsequent
  powered-host pass below demonstrated that the configured lines can be
  acquired and used; retain the original failure as historical evidence until
  the behavior is reproduced across a clean boot.
- NCSI still logs `No channel with link found` and the AST pinctrl driver still
  reports `EPERM` for pin 24. The NCSI MAC requires a host-on, link-level test
  before any DTS pinctrl change.
- KCS, SOL, and KVM capture still need guarded end-to-end tests. Power-good and
  POST transitions were exercised in the powered-host pass below.
- No temperature, DIMM, PSU, chassis-intrusion, PROCHOT/THERMTRIP, or closed-loop
  fan policy has been enabled. Cooling remains full-duty open loop.

## Current SSH audit

The live RAM boot accepts SSH as `root` and has the expected non-host-control
services running: bmcweb, KVM, `obmc-console@ttyVUART0`, KCS/IPMI, Entity
Manager, sensor services, and systemd-networkd. The initial audit reported
only `xyz.openbmc_project.Chassis.Control.Power@0.service` failed; the later
powered-host audit below reports no failed units.

The kernel exposes `/dev/ipmi-kcs3`, `/dev/ttyVUART0`, `/dev/video0`,
`/dev/dri/card0`, and `/dev/fb0`. The dedicated interface is DHCP-configured
at `192.168.178.52`; the NCSI interface has a 100-Mbit link-local address but
still logs `Handler for packet type 0x82 returned -19`, `No channel with link
found`, and the pin-24 `EPERM` errors. After virtual-media cleanup, `/dev/nbd0`
has size zero and no mass-storage gadget is bound.

## Redfish surface audit

The Redfish service root and authenticated system/manager/chassis resources
respond over HTTPS. The TRX40 chassis sensor collection contains 15 resources:
13 voltage channels and two tach channels. Standby rails such as `3VSB` and
`5VSB` return plausible readings; host-switched rails and both tach readings
are `UnavailableOffline` with the host in S5.

The current Redfish `ThermalSubsystem/Fans` and `PowerSubsystem/PowerSupplies`
collections are empty, and `ThermalSubsystem/ThermalMetrics` returns HTTP 500
because no temperature metrics are currently instantiated. This is an
OpenBMC resource-surface deficiency to fix after powered-host sensor discovery;
it is not evidence that the board has no fans or power supplies.

The system resource advertises IPMI serial console and KVM capabilities, but
does not currently publish processor or memory members. PowerState is now
published after the powered-host lifecycle pass below.

## Powered-host S0 pass

After the host was started, the read-only GPIO state stabilized with S3/S5,
PROCHOT/THERMTRIP, and power-good inputs asserted high. Both BMC interfaces
remained up (`eth0` at 1 Gbit/s and the NCSI `eth1` at 100 Mbit/s). The BMC
still exposed only `aspeed_pwm_tacho` and `iio_hwmon`; no temperature or fan
input files and no host/chassis state services appeared.

Focused read-only I²C scans in S0 found unbound responders at I²C1 `0x1d` and
`0x2d`, and I²C3 `0x14`, `0x16`, `0x49`, `0x61`, `0x69`, and `0x73`. The known
EEPROM (`1-0057`) and PCA9545 (`4-0070`) remain the only instantiated child
devices. These addresses require identification against the powered board
before adding DTS nodes or sensor drivers; no type is inferred from address.

## RAM-boot reset during KVM/USB observation

When the host was in S0 and KVM was opened in the browser, the user observed
reduced fan speed and the host logged `usb 9-4: Failed to suspend device, error
-71` (`EPROTO`). Shortly afterward the OpenBMC HTTPS and SSH services
disappeared. The BMC then returned with a new SSH host key and the stock AMI
ASRockRack web root (`Server: lighttpd`, `ASRockRack IPMI`), while the OpenBMC
Redfish resources were absent. This proves a reboot/fallback to the original
flash image; it does not prove whether KVM, USB suspend, or an independent
watchdog caused the reset.

No flash write or persistent change occurred. The next RAM-boot iteration must
capture the complete UART reset/panic/watchdog log and reproduce KVM with the
host USB topology recorded (`lsusb -t` plus the surrounding host `dmesg`) before
claiming KVM or USB stability.

## Live sensor inventory, host in S5

On 2026-07-26 the attached UART was used to inspect the currently running RAM
boot without changing fan duty or host power. The kernel exposed `i2c-0`
through `i2c-5`, `i2c-7`, and `i2c-8`, plus the four PCA9545 child buses
`i2c-40` through `i2c-43`. Only the verified board EEPROM (`1-0057`) and
PCA9545 (`4-0070`) had instantiated child devices; I2C3 had no child device.

The only hwmon providers were `aspeed_pwm_tacho` and `iio_hwmon`. All six PWM
values were `255`; the exposed fan tach inputs were present but returned empty
values. The ADC provider exposed the 13 expected inputs. No NCT6775, DIMM,
motherboard, PSU, or other external temperature hwmon device was present in
this host-off state.

The saved vendor SDR names the remaining records as CPU1/MB/card-side
temperature, eight DDR4 temperatures, two PSU temperatures, twelve fan tach
channels, PSU status/AC-loss/current/input/output telemetry, CPU PROCHOT,
CPU THERMTRIP, chassis intrusion, and eight DRAM ECC counters. Mapping these
records requires repeating the read-only sysfs/I2C inventory with the host
powered in S0; no sensor configuration is inferred from the SDR names alone.

## 2026-07-29 boot and powered lifecycle pass

The attached UART capture is retained as `uart-logs/boot-20260729.txt`. It
shows the corrected RAM image reaching the normal systemd login prompt on
`ttyS4`. Key-based root SSH to `192.168.178.52` then reported Linux
`6.18.39-adcf448-patch-c5a4df5`, the expected board model, no failed systemd
units, and active bmcweb, KVM, VUART console, KCS/IPMI, Entity Manager, and
sensor services.

The power-control service is active in this boot. Its journal recorded
power-button, graceful-off, power-cycle, warm-reset detection, power-good,
and POST-complete transitions. GPIO24/25/26/27 are currently owned by
`power-control` without an access error. D-Bus reports
`HostState.Running` and `Chassis.PowerState.On`; authenticated Redfish reports
the same `PowerState: On` and chassis `State: Enabled`.

With the host powered, FAN1 and FAN3 tach inputs read approximately 1818 and
1824 RPM; the other exported tach inputs still time out. The Redfish fan and
power-supply collections remain empty, and
`/redfish/v1/Chassis/ASRock_Rack_TRX40D8_2N2T/ThermalSubsystem/ThermalMetrics`
still returns HTTP 500. These are now the primary sensor/Redfish gaps rather
than evidence of a general host-state failure.

The ASRock manual documents `TR1` as a 3-pin thermal-sensor header and lists
system-TR temperature sensing in the motherboard hardware monitor. The live
BMC image does not expose a TR1-specific hwmon channel, and the read-only S0
I2C scans found no identified temperature device or `0x4c` device matching the
inherited X570D4U temperature node. The original BMC nevertheless consumed
TR1 for fan-curve control. This makes TR1 a confirmed OpenBMC transport/driver
integration gap; its original Super-I/O, host-IPMI, or other management path
must be traced before adding a driver or Entity Manager sensor.

The BMC kernel has no `/sys/bus/pci/devices` hierarchy. Redfish consequently
returns an empty `PCIeDevices` collection and zero processor and memory
counts. This is a missing host-inventory publication path; it is not evidence
that the AST2500 PCI-to-AHB support is absent. The board layer currently
publishes BMC board inventory, voltages, and fan tachometers only.

The host OS is available again at the read-only audit boundary. Ubuntu reports
an AMD x86_64 system with a Threadripper 3970X; `lspci -nn` lists the NVIDIA
TU102 RTX 2080 Ti, Intel X710-AT2, AST1150 bridge and AST graphics function,
ASM1061 SATA, and two Intel I225-LM NICs. Host hwmon currently exposes only
AMD `k10temp`; no NCT6775 module was loaded during discovery. This gives the
inventory work a concrete host-side source without pretending those devices
are BMC-local PCI objects.

## NCT6796D temperature probe

The powered host left a responding but unbound device at BMC I2C1 address
`0x2d`. A transient device declaration using the kernel's `nct6796` I2C
profile created hwmon channels:

```text
TSI0_TEMP = 36500 mC
TSI1_TEMP = 55750 mC
```

The temporary device was removed after the read-only probe. The permanent DTS
node now declares `nuvoton,nct6796` at I2C1/`0x2d`, so the next image should
instantiate the same driver automatically. These values prove the NCT6796D
transport and driver path, but do not identify which TSI channel is TR1. That
mapping must be correlated with the physical TR1 sensor before naming the
channel or enabling closed-loop fan control.

## BIOS configuration manager probe

The updated image runs `biosconfig-manager` and exposes the standard
`xyz.openbmc_project.BIOSConfig.Manager` object at
`/xyz/openbmc_project/bios_config/manager`. Its `BaseBIOSTable` is empty,
however, and Redfish therefore does not publish `Bios/Attributes`. This is
expected for the manager alone: a PLDM or IPMI/host-firmware BIOS provider must
populate the table. The original vendor BIOS JSON/XML files are not an
OpenBMC provider and are not copied into the image without a transport and
attribute-semantic mapping.

## AMI UEFI inventory boot trace

During a complete host reboot, the installed OpenBMC image kept the dedicated
USB gadget bound with carrier, but `usb0` received no packets and the AMI
inventory D-Bus state remained empty. Linux later enumerated the gadget,
accepted a temporary `169.254.0.18/16` address, reached `169.254.0.17`, and
received HTTP 200 from both the Redfish service root and the AMI inventory
endpoint. The BMC counters advanced during that isolated test. This separates
a working USB/RNDIS/TCP/bmcweb path from a UEFI driver-start problem.

Clean-room disassembly of `RedfishHi` shows that it refuses to initialize
unless `EFI_SIMPLE_NETWORK_PROTOCOL.Mode` reports both
`MediaPresentSupported` and `MediaPresent`. The original BMC's unstripped
`eth.ko` creates a Communications-class USB device with VID:PID `046b:ffb0`
and the strings `American Megatrends Inc.`, `Virtual Ethernet`, and
`1234567890`; the installed OpenBMC image instead exposed `1d6b:0104` with
miscellaneous-device class `ef/02/01`.

The next image mirrors the original descriptor identity. It also satisfies the
second firmware gate: `AmiRedfishDynExt` directly requests the embedded
inventory extension GUID and compares `Id` plus its 32-character MD5 before it
enables `RfInventory`. The raw BIOS extension has MD5
`24e5614de3ead58517b9a1f001f272a8`. bmcweb returns that identity from a
source-restricted native route, avoiding installation or execution of the
vendor extension archive.

The corrected ConfigFS script was also run transiently against the dedicated
gadget while the host OS was online. Linux re-enumerated it as
`046b:ffb0 American Megatrends, Inc. Virtual Ethernet`, reported device class
`02/00/00`, and bound `rndis_host`. With a temporary `169.254.0.18/16` address,
ping to `169.254.0.17` had no loss and HTTPS GETs of `/redfish/v1/` and the AMI
inventory endpoint both returned HTTP 200. The address was removed and the
host interface returned to down after the test.
