/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ami::inventory
{

struct Cpu
{
    uint16_t recordId{};
    uint8_t index{};
    uint16_t coreCount{};
    uint16_t threadCount{};
    uint32_t maxFrequencyMhz{};
    std::string vendor;
    std::string family;
    std::string model;
    std::string stepping;
    std::string brand;
    std::string location;
};

struct Dimm
{
    uint16_t recordId{};
    uint8_t index{};
    uint16_t sizeMiB{};
    uint16_t speedMhz{};
    uint8_t socket{};
    uint8_t channel{};
    uint8_t memoryController{};
    uint8_t slot{};
    std::string manufacturer;
    std::string moduleType;
    std::string dramType;
    std::string voltage;
    std::string serialNumber;
    std::string partNumber;
    std::string location;
};

struct Pci
{
    uint16_t recordId{};
    uint8_t index{};
    uint8_t slotId{};
    uint8_t segment{};
    uint8_t bus{};
    uint8_t deviceFunction{};
    uint16_t vendorId{};
    uint16_t deviceId{};
    uint16_t subsystemVendorId{};
    uint16_t subsystemId{};
    uint8_t revisionId{};
    uint8_t classCode{};
    uint8_t subclassCode{};
    uint8_t programmingInterface{};
    std::string location;
};

struct Inventory
{
    std::vector<Cpu> cpus;
    std::vector<Dimm> dimms;
    std::vector<Pci> pciDevices;
};

/**
 * Parse the AMI System Inventory payload accepted by the vendor BMC's
 * netfn 0x32 command 0x5a handler.  The caller supplies the bytes following
 * the command's three selector bytes.
 */
bool parse(std::span<const uint8_t> payload, Inventory& result,
           std::string& error);

} // namespace ami::inventory
