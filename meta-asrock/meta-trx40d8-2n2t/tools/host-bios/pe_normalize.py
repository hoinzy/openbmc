#!/usr/bin/env python3
"""Canonicalize and explicitly rebase PE/TE firmware executables.

UEFI firmware builders commonly rebase otherwise identical executable sections
when a neighboring FFS file changes size.  Hashing those sections verbatim then
misidentifies placement differences as code differences.  This module applies
the inverse of supported base relocations and clears ImageBase, producing bytes
that are stable across placement changes.

The public rebasing helper is intentionally limited to TE images and requires
the caller to supply the adjusted in-memory data address used by UEFI.  It does
not locate images inside a firmware volume or update enclosing checksums.
"""

import struct


class ExecutableFormatError(ValueError):
    """Raised when an image is malformed or uses an unsupported relocation."""


def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def _require_range(data, offset, size, description):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ExecutableFormatError(
            f"{description} is outside the image: offset=0x{offset:x}, size=0x{size:x}"
        )


def _apply_relocation_delta(data, delta, reloc_offset, reloc_size, rva_to_offset):
    if not reloc_size:
        return
    _require_range(data, reloc_offset, reloc_size, "base relocation directory")
    cursor = reloc_offset
    end = reloc_offset + reloc_size
    while cursor < end:
        _require_range(data, cursor, 8, "base relocation block")
        page_rva, block_size = struct.unpack_from("<II", data, cursor)
        if block_size < 8 or block_size % 2:
            raise ExecutableFormatError(f"invalid relocation block size 0x{block_size:x}")
        _require_range(data, cursor, block_size, "base relocation block")
        for entry_offset in range(cursor + 8, cursor + block_size, 2):
            entry = _u16(data, entry_offset)
            relocation_type = entry >> 12
            if relocation_type == 0:  # IMAGE_REL_BASED_ABSOLUTE padding
                continue
            target_rva = page_rva + (entry & 0x0FFF)
            target_offset = rva_to_offset(target_rva)
            if relocation_type == 3:  # IMAGE_REL_BASED_HIGHLOW
                _require_range(data, target_offset, 4, "HIGHLOW relocation target")
                value = (_u32(data, target_offset) + delta) & 0xFFFFFFFF
                struct.pack_into("<I", data, target_offset, value)
            elif relocation_type == 10:  # IMAGE_REL_BASED_DIR64
                _require_range(data, target_offset, 8, "DIR64 relocation target")
                value = (_u64(data, target_offset) + delta) & 0xFFFFFFFFFFFFFFFF
                struct.pack_into("<Q", data, target_offset, value)
            else:
                raise ExecutableFormatError(
                    f"unsupported base relocation type {relocation_type} at RVA 0x{target_rva:x}"
                )
        cursor += block_size


def _undo_relocations(data, image_base, reloc_offset, reloc_size, rva_to_offset):
    _apply_relocation_delta(data, -image_base, reloc_offset, reloc_size, rva_to_offset)


def _normalize_te(original):
    data = bytearray(original)
    _require_range(data, 0, 40, "TE header")
    if data[:2] != b"VZ":
        raise ExecutableFormatError("invalid TE signature")

    stripped_size = _u16(data, 6)
    stripped_adjustment = stripped_size - 40
    if stripped_adjustment < 0:
        raise ExecutableFormatError(f"invalid TE stripped size 0x{stripped_size:x}")
    image_base = _u64(data, 16)
    relocation_rva, relocation_size = struct.unpack_from("<II", data, 24)

    def rva_to_offset(rva):
        offset = rva - stripped_adjustment
        _require_range(data, offset, 1, f"TE RVA 0x{rva:x}")
        return offset

    if relocation_size:
        relocation_offset = rva_to_offset(relocation_rva)
        _undo_relocations(data, image_base, relocation_offset, relocation_size, rva_to_offset)
    struct.pack_into("<Q", data, 16, 0)
    return bytes(data)


def rebase_te(original, adjusted_image_base):
    """Rebase a TE image to its adjusted in-memory data address.

    A TE header stores the pre-strip image base.  Firmware analysis tools expose
    an adjusted base that adds back ``StrippedSize - sizeof(TE_HEADER)``.  This
    function accepts that adjusted address, updates relocation targets by the
    corresponding delta, and writes the pre-strip value into the TE header.
    """

    data = bytearray(original)
    _require_range(data, 0, 40, "TE header")
    if data[:2] != b"VZ":
        raise ExecutableFormatError("invalid TE signature")
    if adjusted_image_base < 0 or adjusted_image_base > 0xFFFFFFFFFFFFFFFF:
        raise ExecutableFormatError(
            f"adjusted TE image base is out of range: 0x{adjusted_image_base:x}"
        )

    stripped_size = _u16(data, 6)
    stripped_adjustment = stripped_size - 40
    if stripped_adjustment < 0:
        raise ExecutableFormatError(f"invalid TE stripped size 0x{stripped_size:x}")
    if adjusted_image_base < stripped_adjustment:
        raise ExecutableFormatError("adjusted TE image base is below stripped adjustment")

    old_image_base = _u64(data, 16)
    new_image_base = adjusted_image_base - stripped_adjustment
    relocation_rva, relocation_size = struct.unpack_from("<II", data, 24)

    def rva_to_offset(rva):
        offset = rva - stripped_adjustment
        _require_range(data, offset, 1, f"TE RVA 0x{rva:x}")
        return offset

    if relocation_size:
        relocation_offset = rva_to_offset(relocation_rva)
        _apply_relocation_delta(
            data,
            new_image_base - old_image_base,
            relocation_offset,
            relocation_size,
            rva_to_offset,
        )
    struct.pack_into("<Q", data, 16, new_image_base)
    return bytes(data)


def _normalize_pe(original):
    data = bytearray(original)
    _require_range(data, 0, 64, "DOS header")
    if data[:2] != b"MZ":
        raise ExecutableFormatError("invalid DOS signature")
    pe_offset = _u32(data, 0x3C)
    _require_range(data, pe_offset, 24, "PE and COFF headers")
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ExecutableFormatError("invalid PE signature")

    coff_offset = pe_offset + 4
    number_of_sections = _u16(data, coff_offset + 2)
    optional_size = _u16(data, coff_offset + 16)
    optional_offset = coff_offset + 20
    _require_range(data, optional_offset, optional_size, "PE optional header")
    magic = _u16(data, optional_offset)
    if magic == 0x10B:
        image_base_offset = optional_offset + 28
        image_base = _u32(data, image_base_offset)
        directory_count_offset = optional_offset + 92
        directories_offset = optional_offset + 96
        image_base_size = 4
    elif magic == 0x20B:
        image_base_offset = optional_offset + 24
        image_base = _u64(data, image_base_offset)
        directory_count_offset = optional_offset + 108
        directories_offset = optional_offset + 112
        image_base_size = 8
    else:
        raise ExecutableFormatError(f"unsupported PE optional-header magic 0x{magic:x}")

    _require_range(data, directory_count_offset, 4, "PE data-directory count")
    directory_count = _u32(data, directory_count_offset)
    sections_offset = optional_offset + optional_size
    _require_range(data, sections_offset, number_of_sections * 40, "PE section table")
    sections = []
    for index in range(number_of_sections):
        section = sections_offset + index * 40
        virtual_size = _u32(data, section + 8)
        virtual_address = _u32(data, section + 12)
        raw_size = _u32(data, section + 16)
        raw_offset = _u32(data, section + 20)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    size_of_headers = _u32(data, optional_offset + 60)

    def rva_to_offset(rva):
        if rva < size_of_headers:
            _require_range(data, rva, 1, f"PE header RVA 0x{rva:x}")
            return rva
        for virtual_address, mapped_size, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + mapped_size:
                delta = rva - virtual_address
                if delta >= raw_size:
                    raise ExecutableFormatError(f"PE RVA 0x{rva:x} has no file-backed data")
                offset = raw_offset + delta
                _require_range(data, offset, 1, f"PE RVA 0x{rva:x}")
                return offset
        raise ExecutableFormatError(f"PE RVA 0x{rva:x} is not mapped by a section")

    if directory_count > 5:
        _require_range(data, directories_offset, 6 * 8, "PE data directories")
        relocation_rva, relocation_size = struct.unpack_from(
            "<II", data, directories_offset + 5 * 8
        )
        if relocation_size:
            relocation_offset = rva_to_offset(relocation_rva)
            _undo_relocations(data, image_base, relocation_offset, relocation_size, rva_to_offset)

    if image_base_size == 4:
        struct.pack_into("<I", data, image_base_offset, 0)
    else:
        struct.pack_into("<Q", data, image_base_offset, 0)
    return bytes(data)


def normalize_executable(data):
    """Return placement-independent PE/TE bytes for semantic hashing."""

    if data[:2] == b"VZ":
        return _normalize_te(data)
    if data[:2] == b"MZ":
        return _normalize_pe(data)
    raise ExecutableFormatError("executable is neither a PE nor TE image")
