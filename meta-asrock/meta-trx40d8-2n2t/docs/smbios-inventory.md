# Host CPU and memory inventory

The BMC does not enumerate the host PCI bus, so processors and DIMMs must come
from a host inventory transport.  This board's AMI L1.19F BIOS generates a
valid SMBIOS 3.2 table but does not implement the standard OpenBMC MDRv2/blob
upload.  The supported integration is therefore:

```text
Ubuntu host SMBIOS -> KCS /dev/ipmi0 -> OpenBMC /smbios blob
             -> smbios-mdr -> D-Bus inventory -> Redfish
```

`smbios-mdr` and `phosphor-ipmi-blobs` are included in the BMC image.  The
board layer enables the `smbios-ipmi-blob` handler, which receives `/smbios`,
persists it on the BMC, and refreshes its D-Bus CPU and DIMM inventory.

## Verified host data

The running host exposes SMBIOS 3.2 and the BIOS reports `L1.19F` dated
2022-03-31.  The supplied `TRX4D2T1.19F` update file has SHA-256
`f8a52cbba6b8000a8b300de019cbee0e034daf0d1919f34cf44767cd31708a01`, matching
the BIOS version reported by the host.  It contains the AMD AGESA SMBIOS
memory producer (`AmdMemSmbiosV2Pei`) and Type-17 creation code, but no MDRv2
or OpenBMC blob-transfer implementation.

| Item | Live SMBIOS value |
| --- | --- |
| CPU | AMD Ryzen Threadripper 3970X |
| Socket/cores/threads | `SP3r2` / 32 / 64 |
| Memory array | 8 devices, 512 GiB firmware maximum, no ECC |
| Installed memory | 8 x 16 GiB G.Skill `F4-3600C16-16GTRGC` |
| Module details | DDR4 UDIMM, two rank, 1.2 V, configured 3600 MT/s |
| Firmware locators | `P0 CHANNEL A` through `P0 CHANNEL D`, `DIMM 0` and `DIMM 1` per channel |

The manual names the physical sockets `A1 A2 B1 B2 C1 C2 D1 D2`: `A1`, `B1`,
`C1`, and `D1` are blue; the corresponding `2` sockets are white.  Firmware
uses channel plus numeric locators rather than those silk-screen names.  Do
not assert a `DIMM 0 -> A1` or `DIMM 1 -> A2` mapping without a controlled
single-DIMM or firmware-instrumented check; the uploader preserves the
authoritative SMBIOS locators exactly.

The manual documents 256 GiB maximum capacity whereas current SMBIOS Type 16
advertises 512 GiB.  The current live table is published unchanged; the
difference should be treated as a documentation-versus-firmware discrepancy,
not silently normalized by the BMC.

## Test and deploy the host uploader

Copy `tools/smbios-ipmi-upload.py` to the Ubuntu host and first run its local
validation.  The default is a dry run: it reads only the two sysfs SMBIOS
files and contacts neither BMC nor firmware.

```sh
sudo install -Dm0755 smbios-ipmi-upload.py /usr/local/sbin/smbios-ipmi-upload.py
sudo /usr/local/sbin/smbios-ipmi-upload.py --self-test
sudo /usr/local/sbin/smbios-ipmi-upload.py
```

After a BMC image containing this change is active, publish the current table:

```sh
sudo /usr/local/sbin/smbios-ipmi-upload.py --upload
```

This only writes the BMC's SMBIOS inventory cache over the existing KCS
interface.  It neither changes the host BIOS nor resets the host or BMC.
Validate the result with:

```sh
curl -k -u root 'https://<bmc>/redfish/v1/Systems/system'
curl -k -u root 'https://<bmc>/redfish/v1/Systems/system/Processors'
curl -k -u root 'https://<bmc>/redfish/v1/Systems/system/Memory'
```

Once the one-shot upload succeeds, make it persistent after every host boot:

```sh
sudo install -Dm0644 openbmc-smbios-upload.service \
  /etc/systemd/system/openbmc-smbios-upload.service
sudo systemctl daemon-reload
sudo systemctl enable openbmc-smbios-upload.service
sudo systemctl start openbmc-smbios-upload.service
systemctl status openbmc-smbios-upload.service
```

The service is intentionally not installed automatically by the BMC build:
the Ubuntu host lifecycle is outside the BMC firmware image and must be
accepted by the host administrator.
