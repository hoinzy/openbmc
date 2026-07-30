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

`AmiRedfishDynExt` then checks the embedded inventory extension before enabling
`RfInventory`. It requests
`/redfish/v1/DynamicExtension/RedfishExtensions/34E46539-1213-4208-9AB6-2D1C21A35523`
and compares JSON fields `Id` and `Md5Checksum` with the embedded raw file.
Its MD5 is `24e5614de3ead58517b9a1f001f272a8`. bmcweb advertises the native
OpenBMC receiver as that already-installed extension. It does not accept,
extract, or execute the vendor Lua archive.

## Firmware protocol

The implemented transaction follows the firmware:

1. Probe `/redfish/v1/` and `/redfish/v1/Oem/Ami/InventoryData`.
2. Read and update `/redfish/v1/oem/ami/inventory/crc`.
3. POST `multipart/form-data` to the inventory endpoint. The part is named
   `static_file`, its filename is `inventory.json`, and its body is JSON.
4. PATCH the inventory endpoint with `{"BootComplete":true}`.

CRC groups include `CPU`, `DIMM`, and `PCIE`. A firmware upload may omit an
unchanged group. The receiver therefore stages each present category, retains
the last committed value for omitted categories, and publishes only after
`BootComplete`. A present but empty category intentionally clears that category.
The complete committed snapshot and CRC state are persisted under
`/var/lib/ami-host-inventory`.

The CRC endpoint uses the firmware's exact array-of-singletons representation:
`{"GroupCrcList":[{"DIMM":value},{"CPU":value},{"PCIE":value}]}`.

The parser accepts the CPU, DIMM, and PCIe field names emitted by `RfInventory`,
applies a 2 MiB request limit, bounds object counts and strings, and rejects
malformed entries before changing D-Bus. It publishes
`xyz.openbmc_project.Inventory.Item.Cpu`,
`xyz.openbmc_project.Inventory.Item.Dimm`, and
`xyz.openbmc_project.Inventory.Item.PCIeDevice` interfaces for bmcweb.

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
data-initialized state. Hardware validation still requires installing the
image with this final quirk, capturing one BIOS upload, and confirming the
resulting D-Bus and Redfish/WebUI CPU, DIMM, and PCIe resources.

## BIOS configuration lead: GPIO 219

The vendor BMC's `libipmipdkcmds.so.6.1.0` contains a separate SMI mailbox
behind OEM netfn `0x3a`, commands `0xc0` through `0xc5`. Command `0xc2`
(SetSMIUser) stages a BMC request and pulses GPIO 219. BIOS/SMM retrieves that
request through `0xc5` (GetSMIBIOS), returns its response through `0xc4`
(SetSMIBIOS), and the BMC-side client reads it with `0xc3` (GetSMIUser);
`0xc0` and `0xc1` expose mailbox status. This is a strong lead for future BIOS
configuration support and is independent of the RfInventory upload implemented
here.

GPIO 219 has not been electrically or runtime validated under OpenBMC. Future
work should first correlate its line name, polarity, ownership, and SMI timing
with passive captures. Do not toggle it or fuzz the mailbox on a running host
until that evidence exists.
