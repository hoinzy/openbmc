/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <xyz/openbmc_project/BIOSConfig/Manager/common.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace ami::bios
{

using Manager =
    sdbusplus::common::xyz::openbmc_project::bios_config::Manager;
using AttributeType = Manager::AttributeType;
using BoundType = Manager::BoundType;
using AttributeValue = std::variant<int64_t, std::string>;
using AttributeOptions =
    std::vector<std::tuple<BoundType, AttributeValue, std::string>>;
using BaseTable = Manager::base_bios_table_t::value_type;
using PendingAttributes = Manager::pending_attributes_t::value_type;

/**
 * Convert the AMI Redfish BIOS attribute registry and current settings into
 * the standard xyz.openbmc_project.BIOSConfig.Manager BaseBIOSTable format.
 */
bool parse(const std::string& registryJson, const std::string& currentJson,
           BaseTable& table, std::string& registryId, std::string& error);

} // namespace ami::bios
