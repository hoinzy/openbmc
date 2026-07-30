/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace ami::inventory
{
namespace
{

using Json = nlohmann::json;

constexpr size_t maxPayloadSize = 2 * 1024 * 1024;
constexpr size_t maxDepth = 32;
constexpr size_t maxStringSize = 256;
constexpr size_t maxCpuCount = 64;
constexpr size_t maxDimmCount = 256;
constexpr size_t maxPcieCount = 512;
constexpr size_t maxFunctionsPerPcieDevice = 8;

std::string getString(const Json& object,
                      std::initializer_list<std::string_view> names)
{
    for (std::string_view name : names)
    {
        auto member = object.find(name);
        if (member == object.end() || member->is_null())
        {
            continue;
        }
        if (member->is_string())
        {
            std::string value = member->get<std::string>();
            if (value.size() > maxStringSize)
            {
                value.resize(maxStringSize);
            }
            return value;
        }
        if (member->is_number_unsigned())
        {
            return std::to_string(member->get<uint64_t>());
        }
        if (member->is_number_integer())
        {
            return std::to_string(member->get<int64_t>());
        }
    }
    return {};
}

template <typename T>
T getUnsigned(const Json& object, std::initializer_list<std::string_view> names)
{
    for (std::string_view name : names)
    {
        auto member = object.find(name);
        if (member == object.end())
        {
            continue;
        }
        uint64_t value = 0;
        if (member->is_number_unsigned())
        {
            value = member->get<uint64_t>();
        }
        else if (member->is_number_integer())
        {
            const int64_t signedValue = member->get<int64_t>();
            if (signedValue < 0)
            {
                continue;
            }
            value = static_cast<uint64_t>(signedValue);
        }
        else if (member->is_string())
        {
            const std::string text = member->get<std::string>();
            int base = 10;
            std::string_view digits = text;
            if (digits.starts_with("0x") || digits.starts_with("0X"))
            {
                base = 16;
                digits.remove_prefix(2);
            }
            const auto [end, ec] = std::from_chars(
                digits.data(), digits.data() + digits.size(), value, base);
            if (ec != std::errc{} || end != digits.data() + digits.size())
            {
                continue;
            }
        }
        else
        {
            continue;
        }
        if (value <= std::numeric_limits<T>::max())
        {
            return static_cast<T>(value);
        }
    }
    return {};
}

bool getPresent(const Json& object)
{
    auto present = object.find("Present");
    if (present != object.end() && present->is_boolean())
    {
        return present->get<bool>();
    }
    auto status = object.find("Status");
    if (status != object.end() && status->is_object())
    {
        const std::string state = getString(*status, {"State"});
        if (state == "Absent")
        {
            return false;
        }
    }
    return true;
}

bool isOdataLink(const Json& value)
{
    return value.is_object() && value.size() == 1 &&
           value.contains("@odata.id");
}

std::string normalizeHex(const Json& object,
                         std::initializer_list<std::string_view> names,
                         size_t width)
{
    std::string value = getString(object, names);
    if (value.empty())
    {
        return {};
    }

    std::string_view digits = value;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    uint64_t number = 0;
    const auto [end, ec] = std::from_chars(
        digits.data(), digits.data() + digits.size(), number, 16);
    if (ec != std::errc{} || end != digits.data() + digits.size())
    {
        return value;
    }

    constexpr std::array<char, 16> hex = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result(width + 2, '0');
    result[0] = '0';
    result[1] = 'x';
    for (size_t index = 0; index < width; ++index)
    {
        result[result.size() - 1 - index] = hex[number & 0xf];
        number >>= 4;
    }
    return result;
}

std::string makeId(const Json& object, std::string_view prefix, size_t index)
{
    std::string id = getString(object, {"Id", "ID"});
    if (id.empty())
    {
        id = std::string(prefix) + std::to_string(index);
    }
    return id;
}

bool findArrays(const Json& node, const std::set<std::string_view>& names,
                std::vector<const Json*>& arrays, size_t depth,
                std::string& error)
{
    if (depth > maxDepth)
    {
        error = "AMI inventory JSON nesting exceeds the supported limit";
        return false;
    }
    if (node.is_object())
    {
        for (const auto& [name, value] : node.items())
        {
            if (names.contains(name) && value.is_array())
            {
                arrays.emplace_back(&value);
                continue;
            }
            if ((value.is_object() || value.is_array()) &&
                !findArrays(value, names, arrays, depth + 1, error))
            {
                return false;
            }
        }
    }
    else if (node.is_array())
    {
        for (const Json& value : node)
        {
            if ((value.is_object() || value.is_array()) &&
                !findArrays(value, names, arrays, depth + 1, error))
            {
                return false;
            }
        }
    }
    return true;
}

bool appendCpu(const Json& value, size_t index, std::vector<Cpu>& cpus,
               std::string& error)
{
    if (!value.is_object())
    {
        error = "CPU inventory entry is not an object";
        return false;
    }
    Cpu cpu;
    cpu.id = makeId(value, "CPU", index);
    cpu.name = getString(value, {"Name"});
    cpu.socket = getString(value, {"Socket"});
    cpu.manufacturer = getString(value, {"Manufacturer"});
    cpu.model = getString(value, {"Model"});
    cpu.family = getString(value, {"ProcessorType", "Family"});
    cpu.maxSpeedMHz = getUnsigned<uint32_t>(value, {"MaxSpeedMHz"});
    cpu.totalCores = getUnsigned<uint16_t>(value, {"TotalCores"});
    cpu.totalThreads = getUnsigned<uint16_t>(value, {"TotalThreads"});
    cpu.present = getPresent(value);
    cpus.emplace_back(std::move(cpu));
    return true;
}

bool appendDimm(const Json& value, size_t index, std::vector<Dimm>& dimms,
                std::string& error)
{
    if (!value.is_object())
    {
        error = "DIMM inventory entry is not an object";
        return false;
    }
    Dimm dimm;
    dimm.id = makeId(value, "DIMM", index);
    dimm.name = getString(value, {"Name"});
    dimm.locator = getString(value, {"DeviceLocator", "Location"});
    dimm.manufacturer = getString(value, {"Manufacturer"});
    dimm.serialNumber = getString(value, {"SerialNumber"});
    dimm.partNumber = getString(value, {"PartNumber"});
    dimm.memoryType = getString(value, {"MemoryDeviceType", "MemoryType"});
    dimm.formFactor = getString(value, {"BaseModuleType"});
    dimm.errorCorrection = getString(value, {"ErrorCorrection"});
    dimm.capacityMiB = getUnsigned<uint64_t>(value, {"CapacityMiB"});
    dimm.dataWidthBits = getUnsigned<uint16_t>(value, {"DataWidthBits"});
    dimm.busWidthBits = getUnsigned<uint16_t>(value, {"BusWidthBits"});
    dimm.speedMHz =
        getUnsigned<uint16_t>(value, {"OperatingSpeedMhz", "SpeedMHz"});

    auto location = value.find("MemoryLocation");
    if (location != value.end() && location->is_object())
    {
        dimm.socket = getUnsigned<uint8_t>(*location, {"Socket"});
        dimm.memoryController =
            getUnsigned<uint8_t>(*location, {"MemoryController"});
        dimm.channel = getUnsigned<uint8_t>(*location, {"Channel"});
        dimm.slot = getUnsigned<uint8_t>(*location, {"Slot"});
    }
    dimm.present = getPresent(value);
    dimms.emplace_back(std::move(dimm));
    return true;
}

bool appendPcieFunction(const Json& value, size_t index,
                        std::vector<PcieFunction>& functions,
                        std::string& error)
{
    if (!value.is_object())
    {
        error = "PCIe function inventory entry is not an object";
        return false;
    }
    PcieFunction function;
    function.id = makeId(value, "Function", index);
    function.vendorId = normalizeHex(value, {"VendorId"}, 4);
    function.deviceId = normalizeHex(value, {"DeviceId"}, 4);
    function.subsystemVendorId = normalizeHex(value, {"SubsystemVendorId"}, 4);
    function.subsystemId = normalizeHex(value, {"SubsystemId"}, 4);
    function.revisionId = normalizeHex(value, {"RevisionId"}, 2);
    function.classCode = normalizeHex(value, {"ClassCode"}, 6);
    function.deviceClass = getString(value, {"DeviceClass"});
    function.functionType = getString(value, {"FunctionType"});
    functions.emplace_back(std::move(function));
    return true;
}

bool appendPcie(const Json& value, size_t index,
                std::vector<PcieDevice>& devices, std::string& error)
{
    if (!value.is_object())
    {
        error = "PCIe inventory entry is not an object";
        return false;
    }
    // The Systems resource contains a PCIeDevices array of links in addition
    // to the complete devices under Chassis.Links.  Only decode the latter.
    if (isOdataLink(value))
    {
        return true;
    }

    PcieDevice device;
    device.id = makeId(value, "PCIe", index);
    device.name = getString(value, {"Name", "Description"});
    device.location = getString(value, {"Location", "Slot", "Description"});
    device.manufacturer = getString(value, {"Manufacturer"});
    device.model = getString(value, {"Model"});
    device.pcieType = getString(value, {"PCIeType"});
    device.maxPcieType = getString(value, {"MaxPCIeType"});
    device.maxLanes = getUnsigned<uint16_t>(value, {"MaxLanes"});
    device.lanesInUse = getUnsigned<uint16_t>(value, {"LanesInUse"});
    device.present = getPresent(value);

    auto pcieInterface = value.find("PCIeInterface");
    if (pcieInterface != value.end() && pcieInterface->is_object())
    {
        device.pcieType = getString(*pcieInterface, {"PCIeType"});
        device.maxPcieType = getString(*pcieInterface, {"MaxPCIeType"});
        device.maxLanes = getUnsigned<uint16_t>(*pcieInterface, {"MaxLanes"});
        device.lanesInUse =
            getUnsigned<uint16_t>(*pcieInterface, {"LanesInUse"});
    }

    std::vector<const Json*> arrays;
    if (!findArrays(value, {"PCIeFunctions", "PCIEFunctions"}, arrays, 0,
                    error))
    {
        return false;
    }
    size_t functionIndex = 0;
    for (const Json* array : arrays)
    {
        if (device.functions.size() + array->size() > maxFunctionsPerPcieDevice)
        {
            // AMI groups unresolved slot records in one absent 00_00_00
            // placeholder.  It is not a valid PCI multi-function device and
            // must not become an OpenBMC inventory object.
            if (!device.present)
            {
                return true;
            }
            error = "PCIe device contains more than eight functions";
            return false;
        }
        for (const Json& function : *array)
        {
            if (!appendPcieFunction(function, functionIndex++, device.functions,
                                    error))
            {
                return false;
            }
        }
    }
    if (arrays.empty() &&
        !appendPcieFunction(value, 0, device.functions, error))
    {
        return false;
    }
    devices.emplace_back(std::move(device));
    return true;
}

template <typename T, typename Decoder>
bool parseCategory(const Json& document,
                   const std::set<std::string_view>& names, size_t limit,
                   std::optional<std::vector<T>>& output, Decoder&& decoder,
                   std::string& error)
{
    std::vector<const Json*> arrays;
    if (!findArrays(document, names, arrays, 0, error))
    {
        return false;
    }
    if (arrays.empty())
    {
        return true;
    }

    output.emplace();
    size_t index = 0;
    for (const Json* array : arrays)
    {
        if (output->size() + array->size() > limit)
        {
            error = "AMI inventory category contains too many entries";
            return false;
        }
        for (const Json& value : *array)
        {
            if (!decoder(value, index++, *output, error))
            {
                return false;
            }
        }
    }
    return true;
}

Json toJson(const Cpu& cpu)
{
    return {{"Id", cpu.id},
            {"Name", cpu.name},
            {"Socket", cpu.socket},
            {"Manufacturer", cpu.manufacturer},
            {"Model", cpu.model},
            {"ProcessorType", cpu.family},
            {"MaxSpeedMHz", cpu.maxSpeedMHz},
            {"TotalCores", cpu.totalCores},
            {"TotalThreads", cpu.totalThreads},
            {"Present", cpu.present}};
}

Json toJson(const Dimm& dimm)
{
    return {{"Id", dimm.id},
            {"Name", dimm.name},
            {"DeviceLocator", dimm.locator},
            {"Manufacturer", dimm.manufacturer},
            {"SerialNumber", dimm.serialNumber},
            {"PartNumber", dimm.partNumber},
            {"MemoryDeviceType", dimm.memoryType},
            {"BaseModuleType", dimm.formFactor},
            {"ErrorCorrection", dimm.errorCorrection},
            {"CapacityMiB", dimm.capacityMiB},
            {"DataWidthBits", dimm.dataWidthBits},
            {"BusWidthBits", dimm.busWidthBits},
            {"OperatingSpeedMhz", dimm.speedMHz},
            {"Present", dimm.present},
            {"MemoryLocation",
             {{"Socket", dimm.socket},
              {"MemoryController", dimm.memoryController},
              {"Channel", dimm.channel},
              {"Slot", dimm.slot}}}};
}

Json toJson(const PcieDevice& device)
{
    Json functions = Json::array();
    for (const PcieFunction& function : device.functions)
    {
        functions.emplace_back(
            Json{{"Id", function.id},
                 {"VendorId", function.vendorId},
                 {"DeviceId", function.deviceId},
                 {"SubsystemVendorId", function.subsystemVendorId},
                 {"SubsystemId", function.subsystemId},
                 {"RevisionId", function.revisionId},
                 {"ClassCode", function.classCode},
                 {"DeviceClass", function.deviceClass},
                 {"FunctionType", function.functionType}});
    }
    return {{"Id", device.id},
            {"Name", device.name},
            {"Location", device.location},
            {"Manufacturer", device.manufacturer},
            {"Model", device.model},
            {"PCIeType", device.pcieType},
            {"MaxPCIeType", device.maxPcieType},
            {"MaxLanes", device.maxLanes},
            {"LanesInUse", device.lanesInUse},
            {"Present", device.present},
            {"PCIeFunctions", std::move(functions)}};
}

} // namespace

bool parseUpdate(std::string_view payload, Update& result, std::string& error)
{
    result = {};
    error.clear();
    if (payload.empty())
    {
        error = "AMI inventory JSON is empty";
        return false;
    }
    if (payload.size() > maxPayloadSize)
    {
        error = "AMI inventory JSON exceeds the 2 MiB limit";
        return false;
    }

    Json document = Json::parse(payload, nullptr, false);
    if (document.is_discarded() || !document.is_object())
    {
        error = "AMI inventory payload is not a JSON object";
        return false;
    }

    if (!parseCategory(document, {"CPU", "CPUs", "Processors"}, maxCpuCount,
                       result.cpus, appendCpu, error) ||
        !parseCategory(document, {"DIMM", "DIMMs", "Memory"}, maxDimmCount,
                       result.dimms, appendDimm, error) ||
        !parseCategory(document, {"PCIE", "PCIeDevices", "PCIEDevices"},
                       maxPcieCount, result.pcieDevices, appendPcie, error))
    {
        return false;
    }

    auto groupCrcList = document.find("GroupCrcList");
    if (groupCrcList != document.end())
    {
        if (!groupCrcList->is_object())
        {
            error = "AMI inventory GroupCrcList is not an object";
            return false;
        }
        result.crcs.emplace();
        for (const auto& [name, value] : groupCrcList->items())
        {
            if ((name != "CPU" && name != "DIMM" && name != "PCIE") ||
                !value.is_number_unsigned())
            {
                error = "AMI inventory GroupCrcList is invalid";
                return false;
            }
            const uint64_t number = value.get<uint64_t>();
            if (number > std::numeric_limits<uint32_t>::max())
            {
                error = "AMI inventory GroupCrcList is invalid";
                return false;
            }
            result.crcs->emplace(name, static_cast<uint32_t>(number));
        }
    }
    if (!result.cpus && !result.dimms && !result.pcieDevices && !result.crcs)
    {
        error = "AMI inventory JSON contains no supported data";
        return false;
    }
    return true;
}

void merge(Inventory& inventory, Update&& update)
{
    if (update.cpus)
    {
        inventory.cpus = std::move(*update.cpus);
    }
    if (update.dimms)
    {
        inventory.dimms = std::move(*update.dimms);
    }
    if (update.pcieDevices)
    {
        inventory.pcieDevices = std::move(*update.pcieDevices);
    }
}

std::string serialize(const Inventory& inventory)
{
    Json document;
    document["Processors"] = Json::array();
    document["Memory"] = Json::array();
    document["PCIeDevices"] = Json::array();
    for (const Cpu& cpu : inventory.cpus)
    {
        document["Processors"].emplace_back(toJson(cpu));
    }
    for (const Dimm& dimm : inventory.dimms)
    {
        document["Memory"].emplace_back(toJson(dimm));
    }
    for (const PcieDevice& device : inventory.pcieDevices)
    {
        document["PCIeDevices"].emplace_back(toJson(device));
    }
    return document.dump(2);
}

bool parseSnapshot(std::string_view payload, Inventory& result,
                   std::string& error)
{
    Update update;
    if (!parseUpdate(payload, update, error) || !update.cpus || !update.dimms ||
        !update.pcieDevices)
    {
        if (error.empty())
        {
            error = "persistent AMI inventory snapshot is incomplete";
        }
        return false;
    }
    result = {};
    merge(result, std::move(update));
    return true;
}

} // namespace ami::inventory
