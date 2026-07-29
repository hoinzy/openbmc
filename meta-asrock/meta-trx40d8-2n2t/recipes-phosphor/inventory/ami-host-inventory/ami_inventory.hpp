/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ami::inventory
{

struct Cpu
{
    std::string id;
    std::string name;
    std::string socket;
    std::string manufacturer;
    std::string model;
    std::string family;
    uint32_t maxSpeedMHz{};
    uint16_t totalCores{};
    uint16_t totalThreads{};
    bool present{true};
};

struct Dimm
{
    std::string id;
    std::string name;
    std::string locator;
    std::string manufacturer;
    std::string serialNumber;
    std::string partNumber;
    std::string memoryType;
    std::string formFactor;
    std::string errorCorrection;
    uint64_t capacityMiB{};
    uint16_t dataWidthBits{};
    uint16_t busWidthBits{};
    uint16_t speedMHz{};
    uint8_t socket{};
    uint8_t memoryController{};
    uint8_t channel{};
    uint8_t slot{};
    bool present{true};
};

struct PcieFunction
{
    std::string id;
    std::string vendorId;
    std::string deviceId;
    std::string subsystemVendorId;
    std::string subsystemId;
    std::string revisionId;
    std::string classCode;
    std::string deviceClass;
    std::string functionType;
};

struct PcieDevice
{
    std::string id;
    std::string name;
    std::string location;
    std::string manufacturer;
    std::string model;
    std::string pcieType;
    std::string maxPcieType;
    std::vector<PcieFunction> functions;
    uint16_t maxLanes{};
    uint16_t lanesInUse{};
    bool present{true};
};

struct Inventory
{
    std::vector<Cpu> cpus;
    std::vector<Dimm> dimms;
    std::vector<PcieDevice> pcieDevices;
};

struct Update
{
    std::optional<std::vector<Cpu>> cpus;
    std::optional<std::vector<Dimm>> dimms;
    std::optional<std::vector<PcieDevice>> pcieDevices;
};

/**
 * Parse one JSON document emitted by AMI RfInventory. Categories not present
 * in the document remain disengaged so callers can retain the previous data.
 */
bool parseUpdate(std::string_view payload, Update& result, std::string& error);

/** Merge all categories present in update into inventory. */
void merge(Inventory& inventory, Update&& update);

/** Serialize a complete, canonical snapshot for persistent storage. */
std::string serialize(const Inventory& inventory);

/** Parse a canonical snapshot created by serialize(). */
bool parseSnapshot(std::string_view payload, Inventory& result,
                   std::string& error);

} // namespace ami::inventory
