/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_bios.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>

namespace
{

using ami::bios::AttributeType;
using ami::bios::BaseTable;
using ami::bios::BoundType;
using nlohmann::json;

json sampleRegistry()
{
    return {
        {"Id", "BiosAttributeRegistryA2395.1.19.0"},
        {"RegistryEntries",
         {{"Attributes",
           {
               {{"AttributeName", "ENUM001"},
                {"Type", "Enumeration"},
                {"DisplayName", "Enumeration"},
                {"HelpText", "Enumeration help"},
                {"ReadOnly", false},
                {"DefaultValue", "Enabled"},
                {"Value",
                 {{{"ValueName", "Disabled"},
                   {"ValueDisplayName", "Disabled"}},
                  {{"ValueName", "Enabled"},
                   {"ValueDisplayName", "Enabled"}}}}},
               {{"AttributeName", "BOOL001"},
                {"Type", "Boolean"},
                {"DefaultValue", true}},
               {{"AttributeName", "INT001"},
                {"Type", "Integer"},
                {"LowerBound", 0},
                {"UpperBound", 20},
                {"ScalarIncrement", 2},
                {"DefaultValue", 2}},
               {{"AttributeName", "STR001"},
                {"Type", "String"},
                {"MinLength", 1},
                {"MaxLength", 15},
                {"DefaultValue", "default"}},
           }}}}};
}

json sampleCurrent()
{
    return {
        {"AttributeRegistry", "BiosAttributeRegistryA2395.1.19.0"},
        {"Attributes",
         {
             {"ENUM001", "Disabled"},
             {"BOOL001", false},
             {"INT001", 10},
             {"STR001", "current"},
             {"MAPIDS", "firmware-private"},
         }},
    };
}

TEST(AmiBios, ConvertsRegistryAndCurrentSettings)
{
    BaseTable table;
    std::string registryId;
    std::string error;
    ASSERT_TRUE(ami::bios::parse(sampleRegistry().dump(),
                                 sampleCurrent().dump(), table, registryId,
                                 error))
        << error;
    EXPECT_EQ(registryId, "BiosAttributeRegistryA2395.1.19.0");
    ASSERT_EQ(table.size(), size_t{4});

    const auto& enumeration = table.at("ENUM001");
    EXPECT_EQ(std::get<0>(enumeration), AttributeType::Enumeration);
    EXPECT_EQ(std::get<std::string>(std::get<5>(enumeration)), "Disabled");
    EXPECT_EQ(std::get<std::string>(std::get<6>(enumeration)), "Enabled");
    ASSERT_EQ(std::get<7>(enumeration).size(), size_t{2});
    EXPECT_EQ(std::get<0>(std::get<7>(enumeration).front()),
              BoundType::OneOf);

    const auto& boolean = table.at("BOOL001");
    EXPECT_EQ(std::get<0>(boolean), AttributeType::Boolean);
    EXPECT_EQ(std::get<int64_t>(std::get<5>(boolean)), 0);
    EXPECT_EQ(std::get<int64_t>(std::get<6>(boolean)), 1);

    const auto& integer = table.at("INT001");
    EXPECT_EQ(std::get<0>(integer), AttributeType::Integer);
    ASSERT_EQ(std::get<7>(integer).size(), size_t{3});

    const auto& string = table.at("STR001");
    EXPECT_EQ(std::get<0>(string), AttributeType::String);
    ASSERT_EQ(std::get<7>(string).size(), size_t{2});
    EXPECT_FALSE(table.contains("MAPIDS"));
}

TEST(AmiBios, UsesCurrentValueWhenDefaultIsMissing)
{
    json registry = sampleRegistry();
    registry["RegistryEntries"]["Attributes"][3].erase("DefaultValue");
    BaseTable table;
    std::string registryId;
    std::string error;
    ASSERT_TRUE(ami::bios::parse(registry.dump(), sampleCurrent().dump(),
                                 table, registryId, error))
        << error;
    EXPECT_EQ(std::get<std::string>(std::get<6>(table.at("STR001"))),
              "current");
}

TEST(AmiBios, RejectsMismatchedRegistry)
{
    json current = sampleCurrent();
    current["AttributeRegistry"] = "DifferentRegistry";
    BaseTable table;
    std::string registryId;
    std::string error;
    EXPECT_FALSE(ami::bios::parse(sampleRegistry().dump(), current.dump(),
                                  table, registryId, error));
    EXPECT_NE(error.find("different registry"), std::string::npos);
}

TEST(AmiBios, RejectsWrongCurrentValueType)
{
    json current = sampleCurrent();
    current["Attributes"]["BOOL001"] = "false";
    BaseTable table;
    std::string registryId;
    std::string error;
    EXPECT_FALSE(ami::bios::parse(sampleRegistry().dump(), current.dump(),
                                  table, registryId, error));
    EXPECT_NE(error.find("wrong type"), std::string::npos);
}

TEST(AmiBios, ConvertsCapturedFirmwareDataWhenProvided)
{
    const char* registryPath = std::getenv("AMI_BIOS_TEST_REGISTRY");
    const char* currentPath = std::getenv("AMI_BIOS_TEST_CURRENT");
    if (registryPath == nullptr || currentPath == nullptr)
    {
        GTEST_SKIP() << "captured AMI BIOS data was not provided";
    }
    std::ifstream registryFile(registryPath, std::ios::binary);
    std::ifstream currentFile(currentPath, std::ios::binary);
    ASSERT_TRUE(registryFile);
    ASSERT_TRUE(currentFile);
    std::ostringstream registry;
    std::ostringstream current;
    registry << registryFile.rdbuf();
    current << currentFile.rdbuf();

    BaseTable table;
    std::string registryId;
    std::string error;
    ASSERT_TRUE(ami::bios::parse(registry.str(), current.str(), table,
                                 registryId, error))
        << error;
    EXPECT_EQ(registryId, "BiosAttributeRegistryA2395.1.19.0");
    EXPECT_EQ(table.size(), size_t{122});
    EXPECT_EQ(std::get<0>(table.at("CHIPSET000")),
              AttributeType::Enumeration);
    EXPECT_EQ(std::get<std::string>(
                  std::get<5>(table.at("CHIPSET000"))),
              "Onboard VGA");
    EXPECT_EQ(std::get<0>(table.at("SETUP003")),
              AttributeType::Integer);
    EXPECT_EQ(std::get<int64_t>(std::get<5>(table.at("SETUP003"))), 10);
}

} // namespace
