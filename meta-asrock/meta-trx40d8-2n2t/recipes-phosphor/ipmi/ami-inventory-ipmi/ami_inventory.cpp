/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace ami::inventory
{
namespace
{

constexpr size_t headerSize = 0x40;
constexpr size_t recordHeaderSize = 9;
constexpr size_t recordCountOffset = 0x3d;
constexpr size_t maxMessageSize = 64 * 1024;
constexpr size_t maxRecordCount = 64;
constexpr size_t maxPathSize = 0x40;
constexpr uint8_t cpuRecordType = 0x01;
constexpr uint8_t dimmRecordType = 0x08;
constexpr uint8_t pciRecordType = 0x20;

uint16_t readLe16(std::span<const uint8_t> data, size_t offset)
{
    return static_cast<uint16_t>(data[offset]) |
           (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(std::span<const uint8_t> data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::string sanitizeString(std::span<const uint8_t> data, size_t offset,
                           size_t maxLength)
{
    if (offset >= data.size())
    {
        return {};
    }

    const size_t end = std::min(data.size(), offset + maxLength);
    std::string result;
    result.reserve(end - offset);
    for (size_t index = offset; index < end && data[index] != 0; ++index)
    {
        const uint8_t character = data[index];
        result.push_back(character >= 0x20 && character <= 0x7e
                             ? static_cast<char>(character)
                             : '_');
    }
    return result;
}

std::string bytesAsHex(std::span<const uint8_t> data, size_t offset,
                       size_t count)
{
    constexpr std::array<char, 16> hex = {'0', '1', '2', '3', '4', '5', '6',
                                           '7', '8', '9', 'a', 'b', 'c', 'd',
                                           'e', 'f'};
    if (offset > data.size() || count > data.size() - offset)
    {
        return {};
    }

    std::string result;
    result.reserve(count * 2);
    for (size_t index = offset; index < offset + count; ++index)
    {
        result.push_back(hex[data[index] >> 4]);
        result.push_back(hex[data[index] & 0x0f]);
    }
    return result;
}

bool decodeCpu(uint16_t recordId, std::string_view location,
               std::span<const uint8_t> data, Inventory& result,
               std::string& error)
{
    constexpr size_t minimumPayloadSize = 0x0f;
    if (data.size() < minimumPayloadSize)
    {
        error = "CPU record is too short";
        return false;
    }

    Cpu cpu{};
    cpu.recordId = recordId;
    cpu.index = data[0];
    cpu.coreCount = data[1];
    cpu.threadCount = data[2];
    cpu.maxFrequencyMhz = readLe32(data, 0x0b);
    cpu.stepping = sanitizeString(data, 0x03, 8);
    cpu.vendor = sanitizeString(data, 0x0f, 16);
    cpu.family = sanitizeString(data, 0x1f, 30);
    cpu.model = sanitizeString(data, 0x3d, 30);
    cpu.brand = sanitizeString(data, 0x5b, 50);
    cpu.location = std::string(location);
    result.cpus.emplace_back(std::move(cpu));
    return true;
}

bool decodeDimm(uint16_t recordId, std::string_view location,
                std::span<const uint8_t> data, Inventory& result,
                std::string& error)
{
    constexpr size_t minimumPayloadSize = 0x5f;
    if (data.size() < minimumPayloadSize)
    {
        error = "DIMM record is too short";
        return false;
    }

    Dimm dimm{};
    dimm.recordId = recordId;
    dimm.index = data[0];
    dimm.sizeMiB = readLe16(data, 0x01);
    dimm.speedMhz = readLe16(data, 0x03);
    dimm.manufacturer = sanitizeString(data, 0x05, 30);
    dimm.moduleType = sanitizeString(data, 0x23, 10);
    dimm.dramType = sanitizeString(data, 0x2d, 10);
    dimm.voltage = sanitizeString(data, 0x37, 10);
    dimm.serialNumber = bytesAsHex(data, 0x41, 4);
    dimm.partNumber = sanitizeString(data, 0x45, 22);
    dimm.socket = data[0x5b];
    dimm.channel = data[0x5c];
    dimm.memoryController = data[0x5d];
    dimm.slot = data[0x5e];
    dimm.location = std::string(location);
    result.dimms.emplace_back(std::move(dimm));
    return true;
}

bool decodePci(uint16_t recordId, std::string_view location,
               std::span<const uint8_t> data, Inventory& result,
               std::string& error)
{
    constexpr size_t minimumPayloadSize = 0x16;
    if (data.size() < minimumPayloadSize)
    {
        error = "PCI record is too short";
        return false;
    }

    Pci pci{};
    pci.recordId = recordId;
    pci.index = data[0];
    pci.slotId = data[2];
    pci.segment = data[4];
    pci.bus = data[5];
    pci.deviceFunction = data[6];
    pci.vendorId = readLe16(data, 7);
    pci.deviceId = readLe16(data, 9);
    pci.subsystemVendorId = readLe16(data, 11);
    pci.subsystemId = readLe16(data, 13);
    pci.revisionId = data[15];
    pci.programmingInterface = data[16];
    pci.subclassCode = data[17];
    pci.classCode = data[18];
    pci.location = std::string(location);
    result.pciDevices.emplace_back(std::move(pci));
    return true;
}

} // namespace

bool parse(std::span<const uint8_t> payload, Inventory& result,
           std::string& error)
{
    result = {};
    error.clear();

    if (payload.size() < headerSize)
    {
        error = "system inventory header is truncated";
        return false;
    }
    if (payload.size() > maxMessageSize)
    {
        error = "system inventory payload exceeds the supported limit";
        return false;
    }

    const size_t recordCount = payload[recordCountOffset];
    if (recordCount > maxRecordCount)
    {
        error = "system inventory record count exceeds the supported limit";
        return false;
    }

    size_t cursor = headerSize;
    for (size_t recordNumber = 0; recordNumber < recordCount; ++recordNumber)
    {
        if (cursor > payload.size() ||
            recordHeaderSize > payload.size() - cursor)
        {
            error = "system inventory record header is truncated";
            return false;
        }

        const auto record = payload.subspan(cursor);
        const uint16_t recordId = readLe16(record, 0);
        const size_t dataLength = readLe16(record, 2);
        const uint8_t recordType = record[4];
        const size_t pathLength = record[8];
        if (pathLength > maxPathSize)
        {
            error = "system inventory record path exceeds the supported limit";
            return false;
        }
        if (pathLength > std::numeric_limits<size_t>::max() -
                             recordHeaderSize ||
            dataLength > std::numeric_limits<size_t>::max() -
                             recordHeaderSize - pathLength)
        {
            error = "system inventory record length overflows";
            return false;
        }

        const size_t recordLength = recordHeaderSize + pathLength + dataLength;
        if (recordLength > payload.size() - cursor)
        {
            error = "system inventory record payload is truncated";
            return false;
        }

        const std::string location =
            sanitizeString(record, recordHeaderSize, pathLength);
        const auto data = record.subspan(recordHeaderSize + pathLength,
                                         dataLength);
        bool decoded = true;
        switch (recordType)
        {
            case cpuRecordType:
                decoded = decodeCpu(recordId, location, data, result, error);
                break;
            case dimmRecordType:
                decoded = decodeDimm(recordId, location, data, result, error);
                break;
            case pciRecordType:
                decoded = decodePci(recordId, location, data, result, error);
                break;
            default:
                break;
        }
        if (!decoded)
        {
            return false;
        }
        cursor += recordLength;
    }
    return true;
}

} // namespace ami::inventory
