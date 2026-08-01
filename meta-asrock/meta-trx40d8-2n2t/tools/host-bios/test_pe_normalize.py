#!/usr/bin/env python3

import struct
import unittest

from pe_normalize import ExecutableFormatError, normalize_executable, rebase_te


def synthetic_te(image_base=0x100000):
    data = bytearray(0x100)
    stripped_size = 0x120
    stripped_adjustment = stripped_size - 40
    relocation_offset = 0x80
    relocation_rva = relocation_offset + stripped_adjustment
    target_offset = 0x40
    target_rva = target_offset + stripped_adjustment

    data[:2] = b"VZ"
    struct.pack_into("<H", data, 2, 0x014C)
    data[4] = 1
    data[5] = 0x0B
    struct.pack_into("<H", data, 6, stripped_size)
    struct.pack_into("<Q", data, 16, image_base)
    struct.pack_into("<II", data, 24, relocation_rva, 12)
    struct.pack_into("<I", data, target_offset, image_base + 0x1234)
    struct.pack_into("<II", data, relocation_offset, 0, 12)
    struct.pack_into("<H", data, relocation_offset + 8, (3 << 12) | target_rva)
    return bytes(data), target_offset, stripped_adjustment


class PeNormalizeTest(unittest.TestCase):
    def test_te_normalization_reverses_highlow_relocation(self):
        image, target, _ = synthetic_te()
        normalized = normalize_executable(image)
        self.assertEqual(struct.unpack_from("<Q", normalized, 16)[0], 0)
        self.assertEqual(struct.unpack_from("<I", normalized, target)[0], 0x1234)

    def test_te_rebase_preserves_normalized_bytes(self):
        image, target, adjustment = synthetic_te()
        adjusted_base = 0x2345000
        rebased = rebase_te(image, adjusted_base)
        header_base = adjusted_base - adjustment

        self.assertEqual(struct.unpack_from("<Q", rebased, 16)[0], header_base)
        self.assertEqual(
            struct.unpack_from("<I", rebased, target)[0], header_base + 0x1234
        )
        self.assertEqual(normalize_executable(rebased), normalize_executable(image))
        self.assertEqual(rebase_te(rebased, adjusted_base), rebased)

    def test_te_rebase_rejects_non_te_input(self):
        with self.assertRaises(ExecutableFormatError):
            rebase_te(b"not a TE image", 0x1000)


if __name__ == "__main__":
    unittest.main()
