# Host BIOS research tooling

`analyze.py` compares hash-pinned official Castle Peak BIOS images with
UEFIExtract and PSPTool. It is an analysis gate, not a BIOS builder: exit code
`2` means the inputs were analyzed successfully but a safe transplant could
not be proven. It never emits a candidate ROM.

Required raw images:

- ASRock Rack TRX40D8-2N2T `1.19F`
- Gigabyte TRX40 AORUS XTREME `F4`
- Gigabyte TRX40 AORUS XTREME `F7g`
- ASUS ROG Zenith II Extreme `2402`, with the 4 KiB ASUS capsule header removed

Use official downloads and keep the files outside the Git repository. The
expected raw-image SHA-256 values are embedded in the script.

```sh
./analyze.py \
  --asrock /path/to/asrock-1.19f.raw \
  --gigabyte-f4 /path/to/TRX4AOXT.F4 \
  --gigabyte-f7g /path/to/TRX40AORUSXTREME.F7g \
  --asus-2402 /path/to/asus-2402.raw \
  --uefiextract /path/to/uefiextract \
  --psptool /path/to/psptool \
  --output /path/to/empty/output-directory
```

The deep comparison extracts decompressed PE32 and TE bodies and consumes
roughly 200 MiB. Pass `--skip-deep` for the faster structural comparison.

The safety gate must remain closed until all of these are available:

1. A complete CastlePeakPI-SP3r3 1.0.0.F module/dependency manifest.
2. Evidence that the ASRock APCB is compatible with that exact module set.
3. A toolchain that rebuilds all affected firmware volumes and validates every
   AMD and OEM signature without private signing material.
4. Offline validation followed by a duplicate BMC-captured backup and a proven
   BMC restore path.

Do not commit or publish vendor firmware or a derivative BIOS image.
