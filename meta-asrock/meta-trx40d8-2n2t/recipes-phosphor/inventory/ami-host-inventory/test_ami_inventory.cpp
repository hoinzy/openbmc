/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace
{

using ami::inventory::Inventory;
using ami::inventory::Update;
using nlohmann::json;

json sampleInventory()
{
    return {
        {"Systems",
         json::array(
             {{{"Processors",
                json::array({{{"Id", "CPU0"},
                              {"Name", "AMD Ryzen Threadripper 3970X"},
                              {"Socket", "CPU0"},
                              {"ProcessorType", "CPU"},
                              {"Manufacturer", "Advanced Micro Devices, Inc."},
                              {"Model", "AMD Ryzen Threadripper 3970X"},
                              {"MaxSpeedMHz", 4500},
                              {"TotalCores", 32},
                              {"TotalThreads", 64}}})},
               {"Memory",
                json::array({{{"Id", "DIMM_A1"},
                              {"Name", "DIMM A1"},
                              {"MemoryType", "DRAM"},
                              {"MemoryDeviceType", "DDR4"},
                              {"BaseModuleType", "UDIMM"},
                              {"CapacityMiB", 16384},
                              {"DataWidthBits", 64},
                              {"BusWidthBits", 72},
                              {"Manufacturer", "G.Skill"},
                              {"SerialNumber", "12345678"},
                              {"PartNumber", "F4-3600C16-16GTRGC"},
                              {"DeviceLocator", "DIMM_A1"},
                              {"OperatingSpeedMhz", 3200},
                              {"ErrorCorrection", "SingleBitECC"},
                              {"MemoryLocation",
                               {{"Socket", 0},
                                {"MemoryController", 0},
                                {"Channel", 0},
                                {"Slot", 0}}}}})},
               {"PCIeDevices",
                json::array(
                    {{{"Id", "GPU0"},
                      {"Name", "Display controller"},
                      {"Manufacturer", "NVIDIA"},
                      {"Model", "RTX"},
                      {"PCIeType", "Gen4"},
                      {"MaxPCIeType", "Gen4"},
                      {"MaxLanes", 16},
                      {"LanesInUse", 16},
                      {"PCIeFunctions",
                       json::array({{{"Id", "0"},
                                     {"VendorId", "10de"},
                                     {"DeviceId", "1e04"},
                                     {"SubsystemVendorId", "1043"},
                                     {"SubsystemId", "8670"},
                                     {"RevisionId", "a1"},
                                     {"ClassCode", "030000"},
                                     {"DeviceClass", "DisplayController"},
                                     {"FunctionType", "Physical"}}})}}})}}})}};
}

TEST(AmiInventory, ParsesFirmwareCategories)
{
    Update update;
    std::string error;
    ASSERT_TRUE(
        ami::inventory::parseUpdate(sampleInventory().dump(), update, error))
        << error;
    ASSERT_TRUE(update.cpus);
    ASSERT_TRUE(update.dimms);
    ASSERT_TRUE(update.pcieDevices);
    ASSERT_EQ(update.cpus->size(), size_t{1});
    ASSERT_EQ(update.dimms->size(), size_t{1});
    ASSERT_EQ(update.pcieDevices->size(), size_t{1});

    EXPECT_EQ(update.cpus->front().totalCores, 32);
    EXPECT_EQ(update.cpus->front().totalThreads, 64);
    EXPECT_EQ(update.dimms->front().capacityMiB, uint64_t{16384});
    EXPECT_EQ(update.dimms->front().memoryType, "DDR4");
    ASSERT_EQ(update.pcieDevices->front().functions.size(), size_t{1});
    EXPECT_EQ(update.pcieDevices->front().functions.front().vendorId, "0x10de");
    EXPECT_EQ(update.pcieDevices->front().functions.front().classCode,
              "0x030000");
}

TEST(AmiInventory, SparseUpdateRetainsOmittedCategories)
{
    Inventory inventory;
    Update initial;
    std::string error;
    ASSERT_TRUE(
        ami::inventory::parseUpdate(sampleInventory().dump(), initial, error))
        << error;
    ami::inventory::merge(inventory, std::move(initial));

    Update cpuOnly;
    ASSERT_TRUE(ami::inventory::parseUpdate(
        R"({"Group":{"CPU":[{"Id":"CPU0","TotalCores":24}]}})", cpuOnly, error))
        << error;
    ASSERT_TRUE(cpuOnly.cpus);
    EXPECT_FALSE(cpuOnly.dimms);
    EXPECT_FALSE(cpuOnly.pcieDevices);
    ami::inventory::merge(inventory, std::move(cpuOnly));

    ASSERT_EQ(inventory.cpus.size(), size_t{1});
    EXPECT_EQ(inventory.cpus.front().totalCores, 24);
    EXPECT_EQ(inventory.dimms.size(), size_t{1});
    EXPECT_EQ(inventory.pcieDevices.size(), size_t{1});
}

TEST(AmiInventory, PresentEmptyCategoryClearsCategory)
{
    Update update;
    std::string error;
    ASSERT_TRUE(ami::inventory::parseUpdate(R"({"DIMM":[]})", update, error))
        << error;
    ASSERT_TRUE(update.dimms);
    EXPECT_TRUE(update.dimms->empty());
    EXPECT_FALSE(update.cpus);
}

TEST(AmiInventory, PreservesAbsentSlotsAndMultiplePcieFunctions)
{
    const json inventory = {
        {"DIMM", json::array({{{"Id", "DIMM_B1"},
                               {"DeviceLocator", "DIMM_B1"},
                               {"Status", {{"State", "Absent"}}}}})},
        {"PCIE",
         json::array(
             {{{"Id", "NIC0"},
               {"PCIeFunctions",
                json::array(
                    {{{"Id", "0"}, {"VendorId", "8086"}, {"DeviceId", "100e"}},
                     {{"Id", "1"},
                      {"VendorId", "8086"},
                      {"DeviceId", "100f"}}})}}})}};
    Update update;
    std::string error;
    ASSERT_TRUE(ami::inventory::parseUpdate(inventory.dump(), update, error))
        << error;
    ASSERT_TRUE(update.dimms);
    ASSERT_TRUE(update.pcieDevices);
    ASSERT_EQ(update.dimms->size(), size_t{1});
    EXPECT_FALSE(update.dimms->front().present);
    ASSERT_EQ(update.pcieDevices->front().functions.size(), size_t{2});
    EXPECT_EQ(update.pcieDevices->front().functions[1].deviceId, "0x100f");
}

TEST(AmiInventory, SnapshotRoundTrip)
{
    Update update;
    std::string error;
    ASSERT_TRUE(
        ami::inventory::parseUpdate(sampleInventory().dump(), update, error))
        << error;
    Inventory original;
    ami::inventory::merge(original, std::move(update));

    Inventory restored;
    ASSERT_TRUE(ami::inventory::parseSnapshot(
        ami::inventory::serialize(original), restored, error))
        << error;
    ASSERT_EQ(restored.cpus.size(), size_t{1});
    ASSERT_EQ(restored.dimms.size(), size_t{1});
    ASSERT_EQ(restored.pcieDevices.size(), size_t{1});
    EXPECT_EQ(restored.dimms.front().partNumber, "F4-3600C16-16GTRGC");
    ASSERT_EQ(restored.pcieDevices.front().functions.size(), size_t{1});
    EXPECT_EQ(restored.pcieDevices.front().functions.front().deviceId,
              "0x1e04");
}

TEST(AmiInventory, RejectsMalformedAndUnsupportedJson)
{
    Update update;
    std::string error;
    EXPECT_FALSE(ami::inventory::parseUpdate("{", update, error));
    EXPECT_FALSE(
        ami::inventory::parseUpdate(R"({"Systems":[]})", update, error));
    EXPECT_FALSE(ami::inventory::parseUpdate(R"({"CPU":[42]})", update, error));
}

TEST(AmiInventory, RejectsOversizedInputAndCategory)
{
    Update update;
    std::string error;
    std::string oversized(2 * 1024 * 1024 + 1, ' ');
    EXPECT_FALSE(ami::inventory::parseUpdate(oversized, update, error));

    json tooMany;
    tooMany["Processors"] = json::array();
    for (size_t index = 0; index < 65; ++index)
    {
        tooMany["Processors"].emplace_back(json{{"Id", index}});
    }
    EXPECT_FALSE(ami::inventory::parseUpdate(tooMany.dump(), update, error));
}

} // namespace
