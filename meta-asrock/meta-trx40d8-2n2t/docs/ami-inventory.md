# AMI firmware inventory

The board BIOS contains AMI `RfInventory` and `RedfishHi` modules. Clean-room
analysis of BIOS `TRX4D2T1.19F` shows that `RfInventory` generates Redfish-shaped
JSON and uploads it to the BMC through an RNDIS USB host interface. This is not
the AMI OEM KCS command previously assumed by this port.

```text
AMI RfInventory
  -> RNDIS USB (host 169.254.0.18, BMC 169.254.0.17)
  -> HTTPS /redfish/v1/Oem/Ami/InventoryData
  -> ami-host-inventory
  -> standard OpenBMC inventory D-Bus objects
  -> bmcweb CPU, Memory, and PCIe resources
```

No host OS agent, Ubuntu service, SMBIOS conversion, or host filesystem is
required. The BIOS is the data producer, just as it was with the original BMC.

## Firmware startup gates

`RedfishHi` locates `EFI_SIMPLE_NETWORK_PROTOCOL` and requires both
`MediaPresentSupported` and `MediaPresent` before starting its Redfish client.
The original BMC's `eth.ko` exposes USB identity `046b:ffb0`, device class
`02/00/00`, manufacturer `American Megatrends Inc.`, product
`Virtual Ethernet`, and serial `1234567890`.

The AMI `UsbRndisDriverSrc` accepts a CDC data interface with class tuple
`0a/00/00`, but `UsbLanDriverSrc` then requires a CDC Ethernet functional
descriptor (class-specific subtype `0x0f`). It reads `iMACAddress` and
`wMaxSegmentSize` from that descriptor before installing the UEFI network
interface. Linux's standard ConfigFS RNDIS function omits this descriptor.
The original `eth.ko` emits it with the host MAC formatted as 12 hexadecimal
characters and a maximum segment size of 1514 bytes. The board kernel patch
adds the equivalent descriptor to `f_rndis`.

`AmiRedfishDynExt` then checks its embedded extensions before enabling the
firmware services. The inventory path requests
`/redfish/v1/DynamicExtension/RedfishExtensions/34E46539-1213-4208-9AB6-2D1C21A35523`
and compares JSON fields `Id` and `Md5Checksum` with the embedded raw file.
Its MD5 is `24e5614de3ead58517b9a1f001f272a8`. bmcweb advertises the native
OpenBMC receiver as that already-installed extension. The BIOS-default data
service similarly checks extension
`24C5E8D6-7D92-4E54-916E-FEE44013F13F`, with MD5
`f33b77b217dd5fc1deae86d64c5b1e28`. Neither route accepts, extracts, or
executes a vendor Lua archive.

## Firmware protocol

The implemented transaction follows the firmware:

1. Probe `/redfish/v1/` and `/redfish/v1/Oem/Ami/InventoryData`.
2. Read the committed CRC groups. The initial inventory response also supplies
   the `System`, `Chassis`, and `Storage` skeleton consumed by `RfInventory`.
3. POST `multipart/form-data` to the inventory endpoint. The part is named
   `static_file`, its filename is `inventory.json`, and its body is JSON.
4. PATCH the inventory endpoint with `{"BootComplete":true}`.

CRC groups include `CPU`, `DIMM`, and `PCIE`. `RfInventory` includes its
`GroupCrcList` in the uploaded JSON rather than calling the separate CRC write
route. A changed boot uploads detailed CPU, DIMM, and PCIe categories. An
unchanged boot still uploads a small System/Chassis document containing only
the CRC map. The receiver therefore accepts CRC-only sparse updates, retains
omitted hardware categories, and publishes changed inventory only after
`BootComplete`. A present but empty category intentionally clears that
category. The complete committed snapshot and CRC state are persisted under
`/var/lib/ami-host-inventory`.

The compatibility CRC endpoint uses the firmware's array-of-singletons
representation:
`{"GroupCrcList":[{"DIMM":value},{"CPU":value},{"PCIE":value}]}`.

The parser accepts the CPU, DIMM, and PCIe field names emitted by `RfInventory`,
applies a 2 MiB request limit, bounds object counts and strings, and rejects
malformed entries before changing D-Bus. It publishes
`xyz.openbmc_project.Inventory.Item.Cpu`,
`xyz.openbmc_project.Inventory.Item.Dimm`, and
`xyz.openbmc_project.Inventory.Item.PCIeDevice` interfaces for bmcweb. The
service owns an ObjectManager at `/xyz/openbmc_project/inventory`, as required
by bmcweb's processor detail path.

## USB and authentication boundary

`ami-host-interface` creates an RNDIS gadget on
`1e6a0000.usb-vhub:p2`. Port `p1` remains available for the virtual-media
gadget observed during bring-up. The BMC address is the vendor-compatible
`169.254.0.17/16`; the firmware uses `169.254.0.18`.

AMI RedfishHi supports no authentication, Basic authentication, or a Redfish
session depending on BIOS setup. The compatibility routes support all three,
but only when the TCP peer is `169.254.0.18`. The firmware fallback
`HostAutoFW` credential is handled inside those source-restricted routes. It is
deliberately not installed as a PAM, web, or SSH account and cannot be reused
from a management LAN interface.

## Validation boundary

The endpoint paths, multipart names, category behavior, USB addresses, RNDIS
transport, and firmware authentication modes are established from the vendor
firmware. Parser unit tests cover full, sparse, empty, malformed, oversized,
and round-trip updates. A live Linux test on the isolated USB interface reached
`169.254.0.17`, completed an HTTPS request to bmcweb, and incremented the BMC's
`usb0` counters. A transient run of the identity-corrected gadget enumerated
on Linux as `046b:ffb0 American Megatrends, Inc. Virtual Ethernet`, bound to
`rndis_host`, and repeated the successful ping and HTTPS checks.

The first identity-corrected image still omitted the CDC Ethernet functional
descriptor, so AMI UEFI did not install its network interface and emitted no
frames. With that descriptor added, a later full reboot produced pre-OS RNDIS
traffic and isolated a second compatibility issue. Dynamic kernel tracing
recorded:

```text
RESET
INIT
HALT
INIT
HALT
RESET
INIT
HALT
INIT
```

AMI never sends `OID_GEN_CURRENT_PACKET_FILTER` after the final `INIT`. Its
Ethernet frames nevertheless arrive at the BMC: the firmware MAC
`02:1a:11:00:00:18` repeatedly asks by ARP for `169.254.0.17` from
`169.254.0.18`, followed by IPv6 router and neighbor discovery. Linux RNDIS
turns carrier off on `HALT` and normally restores it only after a nonzero
packet-filter request, so every BMC reply is dropped after the final `INIT`.
The 7,014-byte capture has SHA-256
`b994aa3af72291739ab03629b122c9f8cd206849c4246ff48dc091566fc3ba56`.

The board kernel therefore adds an opt-in ConfigFS
`initial_packet_filter`. It is zero by default and does not change conforming
RNDIS functions. `ami-host-interface` sets it to `0x000d` (directed,
all-multicast, and broadcast), causing each `INIT` to restore carrier and the
data-initialized state.

The image containing that quirk was installed and validated on 2026-07-30. A
complete BIOS upload produced 215,111 bytes of JSON with SHA-256
`efb4e7d84cb2b3e1f21c4ca9222335bb5b5c81f02f3689dbd0f22d447eb579db`.
It contained one processor, eight DIMMs, 61 detailed PCIe device records, and
154 PCIe function records. One absent `00_00_00` aggregate represented
unresolved slots and was intentionally skipped; 60 present PCIe devices were
published.

The committed Redfish collections report one processor, eight memory modules,
and 60 PCIe devices. The processor resource reports a 32-core, 64-thread
Threadripper 3970X, and each populated DIMM reports 16 GiB DDR4 at 3200 MHz.
The committed CRCs are:

```text
DIMM = 2117671117
CPU  = 3505128955
PCIE = 305144322
```

The snapshot and CRC files survived a daemon restart. A subsequent BIOS reboot
then completed `GetCrcs -> Stage -> Commit` using the CRC-only sparse payload,
left `Pending=false` and `LastError=""`, preserved the same Redfish counts,
and did not republish unchanged hardware objects. This validates the
BIOS-to-BMC path without a host OS service.

## BIOS configuration

`FirmwareConfigDrv` uses the same UEFI Redfish host interface for BIOS
configuration. It uploads an AMI attribute registry and current settings,
retrieves pending settings from `/redfish/v1/Systems/Self/Bios/SD`, applies
them, deletes the pending document, and republishes current settings. The
board receiver converts the registry to the standard
`xyz.openbmc_project.BIOSConfig.Manager.BaseBIOSTable` instead of maintaining a
second BMC-only settings database.

The live `BiosAttributeRegistryA2395.1.19.0` registry has 122 attributes:
102 enumerations, 15 booleans, two integers, and three strings. AMI's current
document has one additional private `MAPIDS` field, which remains available to
the firmware-compatible endpoint but is deliberately excluded from the
standard table.

The exposed resources are:

- `/redfish/v1/Systems/system/Bios`: 122 current, registry-backed attributes
  and a standard `@Redfish.Settings` link.
- `/redfish/v1/Systems/system/Bios/Settings`: authenticated GET and PATCH of
  `PendingAttributes`.
- `/redfish/v1/Systems/Self/Bios` and `/SD`: AMI-compatible current and
  pending documents restricted to the isolated firmware source or an
  authenticated BMC session.
- `/bios/`: AMI's uploaded setup HTML, JavaScript, CSS, and XML served only to
  an authenticated BMC session.

Parser tests include all five manager attribute types and the captured
122-entry firmware registry. A live safe round trip staged the already-current
`CHIPSET000="Onboard VGA"` value through the standard Settings resource,
observed it through AMI `/SD`, and cleared it. A second test left that same
value pending and rebooted the host. UEFI consumed and deleted the pending
entry before Ubuntu started, republished `current-bios.json`, and left the
standard table at 122 attributes with the same value. This proves that both API
surfaces use `bios-settings-mgr` and that applying settings has no host OS
dependency.

No changed BIOS value has yet been applied. Factory-default reset and BIOS
password actions are also not implemented. The board WebUI now exposes an
administrator-only **Operations -> BIOS configuration** page that embeds the
uploaded `/bios/` application. The parent WebUI permits only same-origin
frames, while the child route uses `SAMEORIGIN` and a route-specific CSP.

The uploaded AMI JavaScript duplicates the complete current-BIOS path when it
constructs its initial configuration request. bmcweb accepts only that exact
authenticated compatibility alias; unauthenticated management-LAN requests
remain rejected. The live WebUI bundle, registry request, current-settings
request, assets, frame headers, and zero-pending state were checked over
HTTPS. A visual browser screenshot remains a separate validation item.

## Independent SMI mailbox lead: GPIO 219

The vendor BMC's `libipmipdkcmds.so.6.1.0` contains a separate SMI mailbox
behind OEM netfn `0x3a`, commands `0xc0` through `0xc5`. Command `0xc2`
(SetSMIUser) stages a BMC request and pulses GPIO 219. BIOS/SMM retrieves that
request through `0xc5` (GetSMIBIOS), returns its response through `0xc4`
(SetSMIBIOS), and the BMC-side client reads it with `0xc3` (GetSMIUser);
`0xc0` and `0xc1` expose mailbox status. This mechanism is independent of the
validated Redfish BIOS-settings exchange and may cover operations not exposed
by `FirmwareConfigDrv`.

GPIO 219 has not been electrically or runtime validated under OpenBMC. Future
work should first correlate its line name, polarity, ownership, and SMI timing
with passive captures. Do not toggle it or fuzz the mailbox on a running host
until that evidence exists.
