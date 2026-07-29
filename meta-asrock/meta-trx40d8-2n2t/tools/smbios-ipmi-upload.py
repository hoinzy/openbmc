#!/usr/bin/env python3
"""Publish the host SMBIOS table to OpenBMC's standard /smbios blob.

Run this on the x86 host after the BMC image includes smbios-mdr and
phosphor-ipmi-blobs.  It uses the in-band KCS device through ipmitool; it does
not connect to the BMC network interface and does not write host firmware.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import subprocess
import sys
from typing import Iterable


OEM = bytes((0xCF, 0xC2, 0x00))
OEM_NETFN = 0x2E
OEM_BLOB_COMMAND = 0x80
BLOB_ID = "/smbios"
MAX_SMBIOS_BLOB_SIZE = 32 * 1024
MAX_WRITE_CHUNK = 48


class BlobProtocolError(RuntimeError):
    """The BMC did not return a valid OpenBMC blob-protocol response."""


def crc16_ccitt(data: bytes) -> int:
    """Match the augmented CRC-16-CCITT implementation in ipmi-blob-tool."""

    crc = 0xFFFF
    for byte in data + b"\0\0":
        for bit in range(7, -1, -1):
            xor = bool(crc & 0x8000)
            crc = (crc << 1) & 0xFFFF
            if byte & (1 << bit):
                crc += 1
            if xor:
                crc ^= 0x1021
    return crc


def fix_checksum(data: bytearray, checksum_offset: int, start: int = 0) -> None:
    """Set one-byte checksum so the selected range has an eight-bit sum of 0."""

    data[checksum_offset] = 0
    data[checksum_offset] = (-sum(data[start:])) & 0xFF


def make_upload_blob(entry_point: bytes, dmi_table: bytes) -> bytes:
    """Build a self-contained SMBIOS table for smbios-mdr.

    The sysfs entry point refers to the DMI table by its physical address.
    smbios-mdr receives a single compact blob instead, so replace that address
    with the byte offset immediately following the entry point and update the
    SMBIOS checksum(s).
    """

    entry = bytearray(entry_point)
    if entry.startswith(b"_SM3_"):
        if len(entry) < 0x18 or entry[6] != len(entry):
            raise ValueError("malformed SMBIOS 3 entry point")
        if len(dmi_table) > struct.unpack_from("<I", entry, 12)[0]:
            raise ValueError("DMI table exceeds SMBIOS 3 entry-point maximum")
        struct.pack_into("<Q", entry, 16, len(entry))
        fix_checksum(entry, 5)
    elif entry.startswith(b"_SM_"):
        if len(entry) < 0x1F or entry[5] != len(entry):
            raise ValueError("malformed SMBIOS 2 entry point")
        if len(dmi_table) > struct.unpack_from("<H", entry, 22)[0]:
            raise ValueError("DMI table exceeds SMBIOS 2 entry-point length")
        struct.pack_into("<I", entry, 24, len(entry))
        fix_checksum(entry, 21, 16)
        fix_checksum(entry, 4)
    else:
        raise ValueError("unsupported SMBIOS entry-point anchor")

    blob = bytes(entry) + dmi_table
    if len(blob) >= MAX_SMBIOS_BLOB_SIZE:
        raise ValueError(
            f"SMBIOS blob is {len(blob)} bytes, exceeding {MAX_SMBIOS_BLOB_SIZE - 1}"
        )
    return blob


class BlobClient:
    def __init__(self, ipmitool: str):
        self.ipmitool = ipmitool

    def _raw(self, request: bytes) -> bytes:
        command = [
            self.ipmitool,
            "raw",
            f"0x{OEM_NETFN:02x}",
            f"0x{OEM_BLOB_COMMAND:02x}",
            *(f"0x{byte:02x}" for byte in request),
        ]
        result = subprocess.run(command, check=False, text=True,
                                capture_output=True)
        if result.returncode:
            raise BlobProtocolError(
                "ipmitool failed: " + (result.stderr.strip() or result.stdout.strip())
            )
        try:
            return bytes.fromhex(result.stdout)
        except ValueError as error:
            raise BlobProtocolError(
                f"ipmitool returned non-hex data: {result.stdout.strip()}"
            ) from error

    def request(self, subcommand: int, payload: bytes = b"") -> bytes:
        request = OEM + bytes((subcommand,))
        if payload:
            request += struct.pack("<H", crc16_ccitt(payload)) + payload
        response = self._raw(request)
        if not response:
            return b""
        if not response.startswith(OEM):
            raise BlobProtocolError(f"unexpected OEM response: {response.hex()}")
        response = response[len(OEM):]
        if not response:
            return b""
        if len(response) < 2:
            raise BlobProtocolError("response is missing its CRC")
        received_crc = struct.unpack_from("<H", response)[0]
        body = response[2:]
        if received_crc != crc16_ccitt(body):
            raise BlobProtocolError("response CRC mismatch")
        return body

    def blob_ids(self) -> list[str]:
        count = self.request(0)
        if len(count) != 4:
            raise BlobProtocolError("invalid blob-count response")
        result = []
        for index in range(struct.unpack("<I", count)[0]):
            value = self.request(1, struct.pack("<I", index))
            if not value.endswith(b"\0"):
                raise BlobProtocolError("blob identifier is not NUL-terminated")
            result.append(value[:-1].decode("ascii"))
        return result

    def upload(self, data: bytes, chunk_size: int) -> tuple[int, int]:
        if BLOB_ID not in self.blob_ids():
            raise BlobProtocolError(
                f"{BLOB_ID} is unavailable; install the BMC smbios-mdr blob handler first"
            )
        session_data = self.request(2, struct.pack("<H", 0x0002) +
                                    BLOB_ID.encode("ascii") + b"\0")
        if len(session_data) != 2:
            raise BlobProtocolError("invalid open-blob response")
        session = struct.unpack("<H", session_data)[0]
        try:
            for offset in range(0, len(data), chunk_size):
                self.request(4, struct.pack("<HI", session, offset) +
                             data[offset:offset + chunk_size])
            self.request(5, struct.pack("<HB", session, 0))
            stat = self.request(9, struct.pack("<H", session))
            if len(stat) < 7:
                raise BlobProtocolError("invalid session-stat response")
            state, size, metadata_len = struct.unpack_from("<HIB", stat)
            if len(stat) != 7 + metadata_len:
                raise BlobProtocolError("invalid session-stat metadata length")
            if not state & 0x0008:
                raise BlobProtocolError(f"SMBIOS blob was not committed (state 0x{state:04x})")
            if size != len(data):
                raise BlobProtocolError(
                    f"BMC committed {size} bytes, expected {len(data)}"
                )
            return session, state
        finally:
            try:
                self.request(6, struct.pack("<H", session))
            except BlobProtocolError:
                pass


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upload", action="store_true",
                        help="write the prepared table to the BMC")
    parser.add_argument("--self-test", action="store_true",
                        help="validate the CRC implementation without contacting the BMC")
    parser.add_argument("--ipmitool", default="ipmitool",
                        help="ipmitool executable (default: %(default)s)")
    parser.add_argument("--entry-point",
                        default="/sys/firmware/dmi/tables/smbios_entry_point")
    parser.add_argument("--dmi-table", default="/sys/firmware/dmi/tables/DMI")
    parser.add_argument("--chunk-size", type=int, default=32,
                        help=("payload bytes per KCS write, 1-%d "
                              "(default: %%(default)s)" % MAX_WRITE_CHUNK))
    return parser.parse_args(argv)


def self_test() -> None:
    vectors = ((b"", 0x1D0F), (b"A", 0x9479), (b"123456789", 0xE5CC))
    for data, expected in vectors:
        if crc16_ccitt(data) != expected:
            raise AssertionError(f"CRC mismatch for {data!r}")
    print("CRC self-test passed")


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        self_test()
        return 0
    if not 1 <= args.chunk_size <= MAX_WRITE_CHUNK:
        raise ValueError(f"chunk size must be between 1 and {MAX_WRITE_CHUNK}")

    entry_point = pathlib.Path(args.entry_point).read_bytes()
    dmi_table = pathlib.Path(args.dmi_table).read_bytes()
    blob = make_upload_blob(entry_point, dmi_table)
    print(
        f"Prepared SMBIOS blob: {len(blob)} bytes "
        f"(entry point {len(entry_point)}, DMI {len(dmi_table)}), "
        f"SHA-256 {hashlib.sha256(blob).hexdigest()}"
    )
    if not args.upload:
        print("Dry run only; pass --upload to send it over KCS.")
        return 0

    session, state = BlobClient(args.ipmitool).upload(blob, args.chunk_size)
    print(f"Uploaded {len(blob)} bytes in session {session}; state 0x{state:04x}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BlobProtocolError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
