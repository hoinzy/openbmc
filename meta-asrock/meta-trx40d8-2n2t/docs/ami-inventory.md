# AMI firmware inventory

The board BIOS contains AMI `SystemInventoryInfo` and `RfInventory` modules.
Its original BMC accepts the resulting inventory on the host KCS interface as
AMI OEM netfn `0x32`, command `0x5a`.  This layer implements a clean-room,
BMC-side receiver for the complete selector-zero message:

```text
AMI BIOS inventory producer -> KCS -> ami-inventory-ipmi
                                      -> D-Bus inventory -> Redfish/WebUI
```

The receiver accepts only the system interface, validates every record before
changing D-Bus, and persists the last valid packet in BMC storage.  It exposes
standard `Inventory.Item.Cpu`, `Inventory.Item.Dimm`, and
`Inventory.Item.PCIeDevice` interfaces, which bmcweb discovers through the
object mapper.  No Ubuntu service, SMBIOS upload tool, or host OS dependency is
part of this design.

The AMI message has a 64-byte header.  Its record count is byte `0x3d`; each
record is a nine-byte header, a bounded location path, and a bounded payload.
The verified record types are CPU (`0x01`), DIMM (`0x08`), and PCI (`0x20`).
The implementation intentionally does not emulate auxiliary vendor selectors
or the original BMC's INI storage format.

Static firmware analysis proves the BIOS-side inventory modules and the
original BMC command/parser.  A future controlled host reboot with the new
image must still capture and confirm the exact runtime request before this is
claimed as hardware-validated.
