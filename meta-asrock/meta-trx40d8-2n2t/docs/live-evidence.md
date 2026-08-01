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

The board image runs the upstream `biosconfig-manager` and exposes
`xyz.openbmc_project.BIOSConfig.Manager` at
`/xyz/openbmc_project/bios_config/manager`. AMI UEFI supplies the missing
provider data over the same isolated RNDIS Redfish interface as inventory.
The board receiver validates the firmware's attribute registry and current
settings, converts all 122 registry entries to `BaseBIOSTable`, and preserves
AMI's private current-only `MAPIDS` value outside the standard table.

The live registry `BiosAttributeRegistryA2395.1.19.0` contains 102
enumerations, 15 booleans, two integers, and three strings. Authenticated
standard Redfish now reports all 122 current attributes at
`/redfish/v1/Systems/system/Bios`, links its Settings object, and maps PATCHes
to `PendingAttributes`. The compatibility endpoint
`/redfish/v1/Systems/Self/Bios/SD` reads and clears the same pending table.
AMI's uploaded setup application is available to authenticated BMC users at
`/bios/`.

On 2026-07-30, a no-value-change test staged
`CHIPSET000="Onboard VGA"` through the standard Settings resource and rebooted
the host. During UEFI, pending attributes changed from one to zero,
`current-bios.json` was republished, and the daemon logged 122 attributes from
the same registry. Ubuntu had not started, proving the setting exchange has no
host OS dependency. The value remained `Onboard VGA`; changed-value behavior,
default reset, and password operations remain unvalidated.

A later RAM-overlay test installed the board-specific WebUI and bmcweb from
commit `cd44e82b7c`. The WebUI bundle contains the administrator-only
`/operations/bios-configuration` route, the `BIOS configuration` navigation
label, and a same-origin `/bios/` frame. The standard root HTML retains
`X-Frame-Options: DENY` and `frame-ancestors 'none'`, but its CSP permits
same-origin child frames. `/bios/` returns
`X-Frame-Options: SAMEORIGIN` with `frame-ancestors 'self'`.

The AMI setup JavaScript constructs the duplicated path
`/redfish/v1/Systems/Self/Bios/redfish/v1/Systems/Self/Bios/` for its initial
configuration GET. The board compatibility route returned HTTP 200 with all
123 firmware fields, including private `MAPIDS`; the same request without a
BMC session returned HTTP 401. The registry route and standard system BIOS
resource each returned 122 public attributes, the Settings resource contained
zero pending attributes, and `/bios/`, `Index.js`, `Index.css`, `Favicon.ico`,
and `RbLogo.png` all returned HTTP 200. No BIOS setting was changed. An
automated visual browser capture remains outstanding.

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

The identity-corrected ConfigFS script was also run transiently against the
dedicated gadget while the host OS was online. Linux re-enumerated it as
`046b:ffb0 American Megatrends, Inc. Virtual Ethernet`, reported device class
`02/00/00`, and bound `rndis_host`. With a temporary `169.254.0.18/16` address,
ping to `169.254.0.17` had no loss and HTTPS GETs of `/redfish/v1/` and the AMI
inventory endpoint both returned HTTP 200. The address was removed and the
host interface returned to down after the test.

The generated image with that corrected identity was then installed and
captured across a complete warm host reboot. A raw `AF_PACKET` recorder was
started on BMC `usb0` before the reboot and stopped only after Linux returned.
There were no Ethernet frames at all between the pre-reboot management
session and Linux bringing up the RNDIS interface: no DHCP, ARP, IPv6
discovery, TCP connection, or HTTPS request originated during UEFI execution.
The first post-reboot frames were Linux multicast-listener messages followed
by the explicit test address and SSH session. The resulting 13 KiB PCAP has
SHA-256
`124284dcf7d54f2f4ccc0c28cb6cd92dca43612fadfca0a16d8bbec508118198`.

After that reboot, Linux again enumerated `046b:ffb0`, bound `rndis_host`, and
reached both compatibility endpoints over `169.254.0.17`. The inventory
daemon nevertheless reported `Pending=false`, an empty CRC map, and no error;
no CPU, DIMM, or PCIe D-Bus objects were created. This rules out the corrected
USB descriptor, BMC address, RNDIS data path, and native bmcweb routes as the
remaining failure boundary. The next investigation must establish why the
UEFI driver is not opening the network device at all, including its
`EFI_SIMPLE_NETWORK_PROTOCOL` media-state gate and any BIOS setup policy that
controls Redfish Host Interface or inventory publication.

Subsequent clean-room analysis of the extracted `UsbRndisDriverSrc` and
`UsbLanDriverSrc` modules identified that activation failure. The RNDIS driver
accepts the gadget's CDC data interface (`0a/00/00`), then publishes an
intermediate protocol. The USB-LAN driver consumes that protocol but aborts
unless class-specific descriptor subtype `0x0f` is present. It uses the CDC
Ethernet descriptor's `iMACAddress` and `wMaxSegmentSize` fields before
installing the UEFI network interface.

Host `lsusb -v` confirmed that the standard Linux ConfigFS RNDIS function
emits CDC header, call-management, ACM, and union descriptors, but no CDC
Ethernet descriptor. In contrast, the original unstripped AMI `eth.ko`
`CreateEthernetDescriptor` function explicitly formats the host MAC as
`%02X%02X%02X%02X%02X%02X`, adds a 13-byte subtype-`0x0f` descriptor, and
sets its maximum segment size to 1514. The board kernel patch now makes
`f_rndis` publish the equivalent descriptor. This explains the complete lack
of pre-OS Ethernet frames: the UEFI network interface was never installed, so
`RedfishHi` could not reach its later media-state check.

## AMI UEFI RNDIS packet-filter trace

After installing the image containing the CDC Ethernet descriptor, a second
warm-reboot capture proved that AMI UEFI now installs and uses its RNDIS
interface. Dynamic debug in `f_rndis.c`, `rndis.c`, and `u_ether.c` recorded
the following control-message sequence:

```text
RESET -> INIT -> HALT -> INIT -> HALT -> RESET -> INIT -> HALT -> INIT
```

There is no `OID_GEN_CURRENT_PACKET_FILTER` request after the final `INIT`.
The last preceding `HALT` calls `netif_carrier_off()` and stops the transmit
queue; standard Linux RNDIS does not reverse those operations on `INIT`.

The simultaneous packet capture contains repeated frames from AMI MAC
`02:1a:11:00:00:18`, including:

```text
ARP, Request who-has 169.254.0.17 tell 169.254.0.18
IPv6 router solicitation
IPv6 neighbor solicitation
```

Receive counters advance on BMC `usb0`, but carrier remains zero and each
attempted BMC response increments `tx_dropped`. No ARP reply reaches UEFI, so
its HTTP inventory client cannot start. The capture is 7,014 bytes with
SHA-256
`b994aa3af72291739ab03629b122c9f8cd206849c4246ff48dc091566fc3ba56`.

Static analysis independently agrees with the trace: the AMI UEFI RNDIS module
constructs INIT, HALT, RESET, QUERY, and KEEPALIVE control messages, but no SET
message (`MessageType == 5`) for `OID_GEN_CURRENT_PACKET_FILTER`.

The compatibility patch adds a per-function ConfigFS
`initial_packet_filter`, disabled by default. The TRX40D8 gadget selects
`0x000d`, which restores directed, all-multicast, and broadcast reception plus
carrier on every `INIT`. This is intentionally board-scoped; standard RNDIS
behavior is unchanged for every function that leaves the attribute at zero.

## AMI host-inventory publication

The packet-filter image was installed and exercised across complete BIOS
boots on 2026-07-30. `RfInventory` reached the source-restricted bmcweb route
from `169.254.0.18`, uploaded a 215,111-byte `inventory.json`, and completed
the staged transaction. The captured JSON has SHA-256
`efb4e7d84cb2b3e1f21c4ca9222335bb5b5c81f02f3689dbd0f22d447eb579db`.

The live payload contains:

| Category | Firmware records | Published present objects |
| --- | ---: | ---: |
| Processor | 1 | 1 |
| DIMM | 8 | 8 |
| PCIe device | 61 detailed | 60 |
| PCIe function | 154 detailed, 89 present | properties on the 60 devices |

The omitted PCIe record is an absent `00_00_00` aggregate with 66 unresolved
function records. It is not a valid PCI multifunction device. Present devices
remain limited to eight functions.

The inventory daemon publishes an ObjectManager at
`/xyz/openbmc_project/inventory`. Authenticated Redfish then reported one
processor, eight memory modules, and 60 PCIe devices. The processor detail
resource reported the AMD Ryzen Threadripper 3970X with 32 cores and 64
threads. A populated DIMM resource reported 16 GiB DDR4 at 3200 MHz and part
number `F4-3600C16-16GTRGC`.

The firmware-supplied CRC values are `DIMM=2117671117`, `CPU=3505128955`, and
`PCIE=305144322`. They are carried inside the uploaded JSON. Both
`inventory.json` and `crc.json` survived an inventory-daemon restart. Their
live SHA-256 values were:

```text
inventory.json c37f8d0053a62e1e98026b6e05f1589246caed10dffc7626075de0f4f08bf34e
crc.json       51d6464657e01b61aaf9ab5dfdc498a9d98d429d83490ce3fca98e471ff084b6
```

On the next BIOS reboot, `RfInventory` performed
`GetCrcs -> Stage -> Commit` with a CRC-only System/Chassis payload. The
daemon accepted this sparse update without replacing or republishing the
hardware categories. It ended with `Pending=false`, an empty `LastError`, and
the same 1/8/60 Redfish counts. No Ubuntu service, SMBIOS converter, or host
filesystem dependency was involved.

## Flashed-image SOL and network identity validation

The `20260731005845` flashed image was exercised with BIOS `L1.19F` and the
host booting Ubuntu. The host completed POST and became reachable over SSH.
Ubuntu remained in systemd's `starting` state only because `k3s-agent` could
not contact `192.168.178.10:6443`; this did not prevent the operating system,
network, or SSH from working. The same boot reported duplicate ACPI I2C
objects from the BIOS and a failure of the host-side `ram-rgb-off.service`.

BIOS and Linux both expose the SOL UART at `0x2f8`, IRQ 3, and 115200 baud.
The BMC-side VUART was configured for the same address and IRQ, but BIOS output
produced only a NUL/break byte. A Linux write longer than the 16-byte UART FIFO
stopped after exactly 16 bytes and received no transmit-empty interrupt. With
the BMC VUART `sirq_polarity` changed from `0` to `1`, a 53-byte marker crossed
the interface in full and the host IRQ 3 count advanced from 9 to 16. The
board DTS therefore selects `IRQ_TYPE_LEVEL_HIGH` for SIRQ 3.

The factory EEPROM contains two valid Ethernet identities. OpenBMC maps
`d0:50:99:f4:22:1e` to the dedicated `eth0` controller and
`d0:50:99:f4:20:d0` to the NCSI `eth1` controller. During this test the
dedicated interface had no carrier and NCSI was active, so DHCP used the
second MAC and assigned `192.168.178.189`; a router reservation for the first
MAC therefore did not apply. The vendor firmware used `bond0` and presented
the first MAC as its management identity. Do not duplicate the first MAC on
both OpenBMC interfaces: decide whether to restore a vendor-compatible bond or
retain distinct per-interface reservations after validating simultaneous
dedicated and NCSI links.

The new `phosphor-pid-control.service` did not start during either observed
host-on transition because `obmc-chassis-poweron@0.target` was not activated.
Consequently it made no PWM changes and did not contribute to the boot issue.
Closed-loop fan control remains disabled in practice until its lifecycle and
the physical fan/PWM mapping are validated.

## Fan-curve WebUI API validation

The first fan-control page implementation used the deprecated broad D-Bus
REST paths under `/xyz/openbmc_project`. Authenticated requests to both the
TR1 curve and TR1 sensor paths returned HTTP 404 on the flashed image, so the
page could not load even though both D-Bus objects existed.

The installed bmcweb already enables OpenBMC's narrow Manager fan-data OEM
interface. An authenticated GET of `/redfish/v1/Managers/bmc/` returned
`Oem.OpenBmc.Fan.StepwiseControllers.TR1_Fan_Curve`, including all seven
20-50 degree targets, 50-100 percent outputs, both 0.5 degree hysteresis
values, the `TR1_TEMP` input, and all six fan zones. A no-op PATCH containing
the same seven `Steps` returned HTTP 200 with Redfish Success messages. The
standard sensor resource at
`/redfish/v1/Chassis/ASRock_Rack_TRX40D8_2N2T/Sensors/temperature_TR1_TEMP`
returned HTTP 200, `Health=OK`, and a live reading of 34 degrees C.

The WebUI store now loads and saves the curve through those Redfish resources
and no longer embeds a legacy D-Bus REST URL. A clean `webui-vue` recipe build
completed all 2,458 tasks, the focused store test passed all three cases, and
the compressed production bundle contains the Manager, TR1 curve, and sensor
identifiers without the old `/xyz` curve path.

The management address returned to `192.168.178.52` after the cable was moved
to the dedicated BMC interface. The earlier `192.168.178.189` address was the
separate NCSI interface and was not evidence of a lost or randomized MAC.

With both interfaces linked to the same LAN, both DHCP routes initially had
metric 1024 and an ARP request for the dedicated `.52` address was answered
with the NCSI `d0:50:99:f4:20:d0` MAC. ICMP remained reliable while new SSH
and HTTPS connections alternated between working and timing out. Setting
`arp_ignore=1`, `arp_announce=2`, and a metric-100 route through dedicated
`eth0` immediately corrected the neighbor entry to `d0:50:99:f4:22:1e`.
Twelve consecutive SSH and HTTPS checks then completed without a failure.

The board package now installs those ARP settings and systemd-networkd
drop-ins with DHCP/RA metrics 100 for dedicated `eth0` and 2048 for NCSI
`eth1`. The exact drop-ins were accepted in a live `/run` test. Reconfiguring
both links released their existing leases; dedicated Ethernet immediately
reacquired `.52`, while NCSI retained IPv6 and a link-local IPv4 address but
had not reacquired its `.189` DHCP lease during the observation window. The
post-update boot must therefore confirm that NCSI reacquires DHCP and remains
a usable fallback without disturbing dedicated-interface stability.

The final incremental image build attempted 6,498 tasks and all succeeded.
The WebUI upload archive is
`obmc-phosphor-image-trx40d8-2n2t-20260731173608.static.mtd.tar`, with SHA-256
`8008735ea649077a05452419e6d607bae5ea125e304a0a089136eead82ddf51a`.
Its root filesystem contains the two route-metric drop-ins, the ARP sysctl,
and the corrected compressed fan-control WebUI bundle.

## Post-flash fan and dual-interface validation

After flashing the `20260731173608` archive, the BMC booted with no failed
systemd units. Dedicated `eth0` reacquired `192.168.178.52`, its ARP entry used
the correct `d0:50:99:f4:22:1e` MAC, and the installed sysctl values were
`arp_ignore=1` and `arp_announce=2`. The generated routes used metric 100 for
dedicated Ethernet. Twelve repeated SSH and HTTPS connection pairs completed
without a failure.

NCSI `eth1` linked at 100 Mbit/s and acquired IPv6, but retained only a
link-local IPv4 address. An ad-hoc tcpdump captured DHCP Discover packets from
`d0:50:99:f4:20:d0` and no Offer packets. A temporary
`RequestBroadcast=yes` drop-in changed the Discover flag to Broadcast and
produced three requests, still with no Offer. The temporary tcpdump binary,
library, and networkd drop-in were removed. The DHCP failure is therefore not
caused by route selection or a unicast-offer requirement; the DHCP server and
upstream NCSI path remain to be checked.

The flashed WebUI bundle contains the Manager fan endpoint,
`TR1_Fan_Curve`, and `temperature_TR1_TEMP`, with no legacy `/xyz` curve
path. Live Redfish returned the complete seven-point curve and a healthy
34-degree-C TR1 reading. A no-op PATCH of the seven `Steps` returned HTTP 200,
and a following GET confirmed the curve was unchanged. The in-app Browser was
not available for a visual rendering check.

The host was running, while `phosphor-pid-control.service` remained inactive
because `obmc-chassis-poweron@0.target` had not been activated. It made no PWM
writes; all six ASPEED PWM outputs remained at the fail-safe value 255. The
live NCT6796 is an I2C device at bus 1 address `0x2d`, driven by
`nct6775-i2c`. Its `in13_input` was 888 mV, which converts to 34 degrees C,
and the same hwmon instance exposed two separate hardware PWM outputs at 153.
Do not start `swampd` until the ASPEED fan/tach mapping, especially the FAN2
pump, has been validated against the physical headers.

## Live TR1 fan-control validation

The fan-control page persisted the requested low-temperature curve through
Redfish: the 20, 25, and 30 degree-C steps were all 30%, followed by 70, 80,
90, and 100% at 35 through 50 degrees C. TR1 read 33 to 34 degrees C with
`Health=OK`, so the expected live target was 30%.

The first failure was service lifecycle rather than curve storage.
`phosphor-pid-control.service` was enabled but inactive because this board
never activated `obmc-chassis-poweron@0.target`. A guarded test first replaced
all curve points with 100%, started `swampd`, and confirmed that all six
ASPEED PWM controls remained at 255. The original user curve was then restored.
PWM1 through PWM3 changed to 76, while FAN1 and FAN3 dropped from about 1820
RPM to about 700 RPM. The water-temperature input remained stable.

The first controller run also exposed an empty-header failure mode. FAN4
through FAN6 have no live tach inputs, so those zones repeatedly entered and
left fail-safe, held PWM4 through PWM6 at 255, and generated more than one
thousand journal messages plus 129 phosphor error records. The persistent
Entity Manager configuration in `/var/configuration/system.json` overrides
the packaged board JSON, including across an Entity Manager restart. A live
test therefore added `MissingIsAcceptable` only to the FAN4, FAN5, and FAN6
controller inputs while preserving the user-edited TR1 curve.

After restarting Entity Manager, the fan and TR1 sensor services, and
`phosphor-pid-control`, all six ASPEED PWM controls settled at 76 (30%). FAN1
and FAN3 settled near 702 and 723 RPM, respectively; TR1 remained healthy at
34 degrees C. The controller stayed active with zero restarts. Its journal
contained only startup discovery messages and no continuing missing-sensor
storm, and the latest phosphor error ID remained unchanged.

The board unit now starts from `multi-user.target` and no longer conflicts
with the chassis-powered-off target. This keeps coolant control independent
of host power-state target behavior. The board configuration marks only the
known telemetry-free pump and empty grouped headers as missing-acceptable;
FAN1 and FAN3 retain tach-based fail-safe protection. A new image is required
to make the service lifecycle change persistent across BMC reboot. The unit
also runs `trx40d8-fan-full-speed` immediately before controller startup and
after controller shutdown. A live stop/start test confirmed that the helper
set all six PWM channels to 255 before `swampd` resumed curve control.

Focused `entity-manager` and `phosphor-pid-control` builds completed all 2,497
tasks. The final `webui-vue`, controller, and full-image pass completed all
6,499 tasks, including package QA. The resulting upload archive is
`obmc-phosphor-image-trx40d8-2n2t-20260731183440.static.mtd.tar`, size
29,726,720 bytes, SHA-256
`a82e667cf4e759b16f4ba8574b068c6837e241fa02520f962ee27a7c1892a6da`.
Its SquashFS contains the multi-user target symlink, the pre/post full-speed
unit hooks and helper, and the four missing-acceptable controller lists.

## Host serial-console root cause and proposed fix

The host firmware's `TerminalSrc` DXE driver reads the two serial-redirection
switches from Setup offsets `0xd3` and `0xd4`, then requires a discoverable
ACPI PNP0501 `EFI_SERIAL_IO_PROTOCOL` device. Both switches were confirmed as
one in the live 563-byte Setup variable after enabling console redirection for
COM1 and COM2, but `ConOut`, `ConOutDev`, `ErrOut`, and `ErrOutDev` still held
only GPU device paths. The BMC received no BIOS text during that boot.

A host-side probe then found the OpenBMC VUART at `0x2f8`, but every AST2500
Super I/O register at configuration ports `0x4e`/`0x4f` returned `0xff`.
Current OpenBMC U-Boot deliberately sets SCU strap bit 20 to disable that
interface unless `CONFIG_ASPEED_ENABLE_SUPERIO` is selected. The live BMC
showed SCU70 `0x5111d246`, with the disable bit set. This explains why the AMI
SioDxe driver cannot create the serial device even though redirection is
enabled in Setup; the VUART provides bytes at the legacy UART address but does
not emulate the Super I/O configuration protocol expected by this BIOS.

The first board-scoped fix followed the existing OpenBMC ASRock E3C256D4I
precedent by enabling the AST2500 Super I/O in U-Boot. It also replaced the
VUART with BMC UART4 and routed physical host IO2 in both directions to UART4.
That transport choice was subsequently disproved by the powered test below.

This is not security-neutral. OpenBMC classifies the AST2500 built-in Super
I/O as a dangerous hardware backdoor because the host can use it to read the
BMC address space. The opt-in is therefore machine-specific and U-Boot still
disables iLPC2AHB, P2A/PCIe BMC access, X-DMA, and LPC2AHB. A post-flash test
must verify both working BIOS SOL and the effective isolation registers before
this change is proposed upstream.

The targeted U-Boot, kernel, and obmc-console build confirmed both Super I/O
Kconfig symbols in the generated U-Boot configuration, produced the updated
board DTB, and packaged only `server.ttyS3.conf` with the IO2/UART4 route. The
following full image build attempted 6,498 tasks and all succeeded. Its WebUI
upload archive is
`obmc-phosphor-image-trx40d8-2n2t-20260731200643.static.mtd.tar`, size
29,726,720 bytes, SHA-256
`5b647ba50c280879118e2f74e3d93acd63bbac87746c7280406a2c43bd716483`.

## Super I/O cold-start and SOL transport validation

A BMC firmware update and software reset left SCU70 at `0x5111d246`: U-Boot
no longer set the Super I/O disable bit, but the value written by the previous
firmware remained latched. A complete removal of AC and standby power restarted
the BMC with SCU70 `0x5101d246`, proving that the true power-on reset was needed
once to clear bit 20.

On the following host boot, the AST2500 configuration interface at `0x4e`/`0x4f`
responded to its `0xa5`, `0xa5` unlock sequence. BIOS had enabled LDN 02 at
`0x3f8`, IRQ 4 and LDN 03 at `0x2f8`, IRQ 3. Linux likewise enumerated two
PNP0501 devices as `ttyS0` and `ttyS1`. This validates the U-Boot Super I/O
change and the original `TerminalSrc` diagnosis.

The UART4/IO2 transport did not carry data. During a controlled Linux write,
the host `ttyS1` transmit count advanced by 16 bytes while BMC `ttyS3` remained
at zero received bytes. Sweeping every UART4 receive source exposed by the
ASPEED routing driver also produced zero bytes, and no BIOS or POST output was
captured. By contrast, the earlier VUART test carried a complete marker once
SIRQ 3 used active-high polarity. The board therefore keeps Super I/O enabled
for BIOS discovery but restores VUART at `0x2f8`, SIRQ 3 as the SOL data path.
The DTS specifies `IRQ_TYPE_LEVEL_HIGH` explicitly instead of relying on the
driver's default active-low SIRQ polarity.

Targeted `linux-aspeed` and clean `obmc-console` builds succeeded. The generated
DTB contains `aspeed,lpc-io-reg = <0x2f8>` and
`aspeed,lpc-interrupts = <3 IRQ_TYPE_LEVEL_HIGH>`, while the packaged console
configuration provides only `server.ttyVUART0.conf`. The subsequent full image
build attempted 6,498 tasks and all succeeded. Its WebUI upload archive is
`obmc-phosphor-image-trx40d8-2n2t-20260731210414.static.mtd.tar`, size
29,726,720 bytes, SHA-256
`203f9f24d54364651a473c13207b8bf8dd1eb6cb9d270a54fd8fe3d9d943b66a`.

That image was then installed and validated after the complete AC/standby
power cycle. SCU70 remained `0x5101d246`, `obmc-console@ttyVUART0` was active,
and the live VUART reported LPC address `0x2f8`, SIRQ 3, and active-high
polarity. A console client attached before the host reset captured the AMI
banner, BIOS `L1.19F`, POST-code progression, the UEFI setup prompt, and the
Linux EFI stub. The VUART receive counter increased from 0 to 3,604 bytes and
the host subsequently returned over SSH. This is the first end-to-end proof
that the flashed OpenBMC image carries BIOS SOL across a cold-started AST2500.

The same boot also disproved the current RGB service's ownership assumption.
The BMC-side `ram-rgb-off.service` remained active, but reads of mux `0x71`
NACKed on every exposed I2C adapter while the host was on. The legacy Ubuntu
helper also failed because OpenBMC does not implement the vendor firmware's
IPMI Master Write-Read command `0x52`. The BMC-native implementation therefore
still needs the vendor bus-ownership or hardware-selector transition to be
identified and must not yet be described as validated across a cold boot. The
obsolete Ubuntu `ram-rgb-off.service` was disabled after this result so that
the unsupported vendor command is no longer retried from the host.

Follow-up reverse engineering identified that transition in the vendor
`libipmipdk.so.6.23.0`. `ASRR_QuickSwithcTask` uses GPIO-handle slot 7
(`set_gpio_data_high`) and slot 8 (`set_gpio_data_low`) on AST GPIO 75 (J3),
while vendor platform initialization first configures J3 as a low output. On
the live OpenBMC image, holding otherwise-unused GPIO 75 high made mux `0x71`
immediately readable on I2C7; all 20 consecutive probes returned its current
selection byte `0x07`. With the route held high, the BMC-native utility
validated each ENE signature, programmed both channels, and independently
read register `0x8021` back as `0x00` on channels `0x01` and `0x02`. Releasing
the GPIO returned it to its original input state.

The service now reproduces this quick-switch operation for each programming
attempt and releases the route afterward. Monitor mode waits for the standard
OpenBMC host state to become `Running`, programs once, and re-arms whenever the
host state changes. This avoids permanently taking the management bus away
from host firmware while still removing the former Ubuntu/IPMI dependency.

The candidate service was exercised live in both one-shot and monitor modes.
Each path programmed both banks, returned success, and left GPIO 75 as an
unused input after the transaction. The targeted `ram-rgb-off` build attempted
1,096 tasks and passed package QA and SPDX generation. A subsequent j64 image
build attempted 6,516 tasks and all succeeded. The resulting WebUI/Redfish
archive is
`obmc-phosphor-image-trx40d8-2n2t-20260731233117.static.mtd.tar`, size
29,736,960 bytes, SHA-256
`0e63eaab1f08c24a1134651b57de7fa75bb87ce74e1458d88b9cccd5d037b92f`.
The script extracted from that image has SHA-256
`36153768e5fade95713c147788d9fd0d4b9afbd2a275c8e71463f195aad8487a`,
identical to the live-tested candidate.

The installed image was then tested across the controller reset boundary,
rather than with a warm host reboot. The RGB service was stopped, the host
reached chassis `PowerState.Off`, and the main rails remained off for 30
seconds before power-on. Before restarting the service, independent reads of
ENE register `0x8021` returned reset mode `0x05` on both mux channels. The
installed service completed both banks after the host returned to `Running`;
independent post-service reads returned `0x00` on both channels. GPIO 75 was
again an unused input, the service remained active, and Ubuntu returned over
SSH. This proves the BMC-native path reapplies Off mode after a real DIMM
controller reset; persistence across a warm reboot is not being mistaken for
service operation.

The cold-boot fan discrepancy was an upgrade-overlay problem rather than a
controller defect. The new SquashFS board JSON contained
`MissingIsAcceptable` for FAN4 through FAN6, but an older file at
`/run/initramfs/rw/cow/usr/share/entity-manager/configurations/asrock/`
masked it. Entity Manager therefore published the property only for FAN2 and
PWM4 through PWM6 remained at 255. The old upper file was copied to
`/var/lib/entity-manager-backups/trx40d8-2n2t.pre-upgrade-20260731.json`, then
replaced with the verified SquashFS configuration. After Entity Manager had
reprobed the board and `phosphor-pid-control` restarted, its startup dump marked
the telemetry-free fan/PWM pairs with `?`, all six zones left fail-safe, and
all six ASPEED PWM values settled at 153 (60%) for the live 32-degree-C TR1
reading. Redfish exposed 33 sensors and a no-op WebUI-equivalent fan-curve
PATCH returned HTTP 200.

The source now includes `trx40d8-config-migration.service` to handle this
boundary on later upgrades. A schema-version change makes it capture the four
writable TR1 curve properties, retain the first old upper JSON for recovery,
overwrite the existing upper inode with the new image JSON, restart Entity
Manager, and restore the user curve before fan control starts. It deliberately
overwrites the inode rather than unlinking it: the live OverlayFS retained a
stale merged dentry after a direct unlink until the corrected upper file was
explicitly rebound.

The targeted j64 build of `trx40d8-config-migration`, `phosphor-pid-control`,
and `packagegroup-asrock-apps` attempted 3,007 tasks and all succeeded. After
the recovery-copy addition, the migration package was rebuilt independently;
all 2,388 tasks succeeded, including package QA, IPK generation, and SPDX
generation. The package contains its executable, schema marker, systemd unit,
and enable preset, and `packagegroup-asrock-apps-system` depends on it. The
build host's AppArmor user-namespace restriction was disabled only while
BitBake ran and restored to its original value afterward.

A final live check found no failed BMC units. Entity Manager, fan control, and
the VUART console service were active; Redfish exposed 33 sensors and reported
`TR1 TEMP` healthy at 34 degrees C. All six PWM outputs remained at 153 of 255
(60 percent), and the lower, upper, and merged board JSON files had the same
SHA-256 digest.

## KVM framebuffer-selection investigation

The flashed OpenBMC image's video path was validated end to end. `obmc-ikvm`
used `/dev/video0`, the mainline ASPEED video driver reported `HOST VGA`,
signal lock, 1024x768 input, and approximately 29 frames per second, and the
Web UI remained connected. Chrome DevTools captured visible AMI POST content
at 720x400 and 800x600 on multiple reboots. The canvas changed to a completely
black 1024x768 frame only when Ubuntu initialized its display drivers.

The host exposed NVIDIA `01:00.0` and ASPEED `46:00.0`. Linux registered
`simpledrmdrmfb` as `fb0` below the NVIDIA PCI device and `astdrmfb` as `fb1`.
All virtual consoles initially mapped to `fb0`; the ASPEED CRTC and its
1024x768 framebuffer were active but contained no console. Temporarily mapping
VT1 to `fb1` with `FBIOPUT_CON2FBMAP` immediately made the Ubuntu login prompt
visible in OpenBMC KVM. This proves that the ASPEED capture hardware, kernel
driver, `obmc-ikvm`, WebSocket, and browser canvas are all working.

IFR extraction and IDA analysis of BIOS L1.19F explain why the firmware setting
appears contradictory. `Primary Graphics Adapter` is `Setup[0x1d5]` and was
correctly set to `Onboard VGA`; `Onboard VGA` is `Setup[0x1d4]` and was enabled.
Later, AMI's GOP policy enumerates GOP handles and writes an
`AmiGopOutputDp` device path. Its hidden `Output Select` value is
`Setup[0x107]`. The firmware-generated setup data listed only `GPU Board` and
`NVIDIA GPU UEFI Driver`, so the NVIDIA GOP still supplied Linux's EFI
framebuffer after ASPEED had displayed POST.

Enabling CSM and setting `Launch Video OpROM Policy` to `Do not launch` was
tested as a firmware-only workaround. The host remained in early POST with an
unlocked 640x480 input and no network, so both settings were reverted before a
forced restart. The baseline settings and normal boot were recovered; this is
not a viable workaround.

For the Ubuntu build host, the proven persistent workaround is an isolated
GRUB drop-in at `/etc/default/grub.d/99-aspeed-kvm.cfg`:

```sh
GRUB_CMDLINE_LINUX_DEFAULT="${GRUB_CMDLINE_LINUX_DEFAULT:+${GRUB_CMDLINE_LINUX_DEFAULT} }fbcon=map:1"
```

After `update-grub` and a reboot, `/proc/cmdline` contained `fbcon=map:1`, VT1
through VT3 mapped to `fb1`, and Chrome DevTools captured the live Ubuntu
console at 1024x768. This host setting is deliberately not part of the
OpenBMC image. A host-independent firmware repair would require the BIOS to
publish/select an ASPEED UEFI GOP (or otherwise leave ASPEED as the EFI boot
framebuffer); OpenBMC cannot redirect or capture a framebuffer owned by the
discrete NVIDIA device.
