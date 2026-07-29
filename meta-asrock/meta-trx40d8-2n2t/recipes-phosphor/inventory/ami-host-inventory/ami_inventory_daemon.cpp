/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view serviceName = "xyz.openbmc_project.AmiHostInventory";
constexpr std::string_view controlPath =
    "/xyz/openbmc_project/inventory/ami_host";
constexpr std::string_view controlInterface =
    "xyz.openbmc_project.AmiHostInventory";
constexpr std::string_view inventoryRoot =
    "/xyz/openbmc_project/inventory/system/chassis/motherboard";
constexpr std::string_view itemInterface = "xyz.openbmc_project.Inventory.Item";
constexpr std::string_view assetInterface =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
constexpr std::string_view locationInterface =
    "xyz.openbmc_project.Inventory.Decorator.LocationCode";
constexpr std::string_view cpuInterface =
    "xyz.openbmc_project.Inventory.Item.Cpu";
constexpr std::string_view dimmInterface =
    "xyz.openbmc_project.Inventory.Item.Dimm";
constexpr std::string_view pcieInterface =
    "xyz.openbmc_project.Inventory.Item.PCIeDevice";
const fs::path stateDirectory = "/var/lib/ami-host-inventory";
const fs::path snapshotPath = stateDirectory / "inventory.json";
const fs::path crcPath = stateDirectory / "crc.json";
constexpr size_t maxSnapshotSize = 2 * 1024 * 1024;

std::string sanitizePathComponent(std::string_view value, size_t index)
{
    std::string output;
    output.reserve(value.size() + 12);
    for (unsigned char character : value)
    {
        if (std::isalnum(character) != 0 || character == '_')
        {
            output.push_back(static_cast<char>(character));
        }
        else
        {
            output.push_back('_');
        }
    }
    if (output.empty())
    {
        output = "unknown";
    }
    output += "_" + std::to_string(index);
    return output;
}

std::string dimmTypeToDbus(std::string_view value)
{
    constexpr std::string_view prefix =
        "xyz.openbmc_project.Inventory.Item.Dimm.DeviceType.";
    constexpr std::string_view values[] = {
        "DDR",   "DDR2", "DDR3", "DDR4", "DDR5", "DRAM", "SDRAM",
        "EDRAM", "SRAM", "RAM",  "ROM",  "HBM",  "HBM2", "HBM3"};
    for (std::string_view candidate : values)
    {
        if (value == candidate)
        {
            return std::string(prefix) + std::string(candidate);
        }
    }
    return std::string(prefix) + "Unknown";
}

std::string formFactorToDbus(std::string_view value)
{
    constexpr std::string_view prefix =
        "xyz.openbmc_project.Inventory.Item.Dimm.FormFactor.";
    constexpr std::string_view values[] = {
        "RDIMM", "UDIMM", "SO_DIMM", "LRDIMM", "Mini_RDIMM", "Mini_UDIMM"};
    for (std::string_view candidate : values)
    {
        if (value == candidate)
        {
            return std::string(prefix) + std::string(candidate);
        }
    }
    return {};
}

std::string eccToDbus(std::string_view value)
{
    constexpr std::string_view prefix =
        "xyz.openbmc_project.Inventory.Item.Dimm.Ecc.";
    constexpr std::string_view values[] = {"NoECC", "SingleBitECC",
                                           "MultiBitECC", "AddressParity"};
    for (std::string_view candidate : values)
    {
        if (value == candidate)
        {
            return std::string(prefix) + std::string(candidate);
        }
    }
    return {};
}

std::string generationToDbus(std::string value)
{
    constexpr std::string_view prefix =
        "xyz.openbmc_project.Inventory.Item.PCIeSlot.Generations.";
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) {
                                   return std::isspace(c) != 0 || c == '_';
                               }),
                value.end());
    if (value.starts_with("PCIe"))
    {
        value.erase(0, 4);
    }
    if (value.starts_with("Gen"))
    {
        value.erase(0, 3);
    }
    if (value == "1" || value == "2" || value == "3" || value == "4" ||
        value == "5" || value == "6")
    {
        return std::string(prefix) + "Gen" + value;
    }
    return std::string(prefix) + "Unknown";
}

class InventoryPublisher
{
  public:
    explicit InventoryPublisher(sdbusplus::asio::object_server& server) :
        objectServer(server)
    {}

    void publish(const ami::inventory::Inventory& inventory)
    {
        clear();
        for (size_t index = 0; index < inventory.cpus.size(); ++index)
        {
            publishCpu(inventory.cpus[index], index);
        }
        for (size_t index = 0; index < inventory.dimms.size(); ++index)
        {
            publishDimm(inventory.dimms[index], index);
        }
        for (size_t index = 0; index < inventory.pcieDevices.size(); ++index)
        {
            publishPcie(inventory.pcieDevices[index], index);
        }
    }

  private:
    using DbusInterface = std::shared_ptr<sdbusplus::asio::dbus_interface>;

    sdbusplus::asio::object_server& objectServer;
    std::vector<DbusInterface> interfaces;

    DbusInterface addInterface(const std::string& path,
                               std::string_view interface)
    {
        DbusInterface result =
            objectServer.add_interface(path, std::string(interface));
        interfaces.emplace_back(result);
        return result;
    }

    void addItem(const std::string& path, const std::string& name, bool present)
    {
        DbusInterface item = addInterface(path, itemInterface);
        item->register_property("PrettyName", name);
        item->register_property("Present", present);
        item->initialize();
    }

    void addAsset(const std::string& path, const std::string& manufacturer,
                  const std::string& model, const std::string& partNumber,
                  const std::string& serialNumber)
    {
        DbusInterface asset = addInterface(path, assetInterface);
        asset->register_property("Manufacturer", manufacturer);
        asset->register_property("Model", model);
        asset->register_property("PartNumber", partNumber);
        asset->register_property("SerialNumber", serialNumber);
        asset->initialize();
    }

    void addLocation(const std::string& path, const std::string& location)
    {
        if (location.empty())
        {
            return;
        }
        DbusInterface interface = addInterface(path, locationInterface);
        interface->register_property("LocationCode", location);
        interface->initialize();
    }

    void publishCpu(const ami::inventory::Cpu& cpu, size_t index)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_cpu_" +
                                 sanitizePathComponent(cpu.id, index);
        const std::string name = cpu.name.empty()
                                     ? (cpu.model.empty() ? cpu.id : cpu.model)
                                     : cpu.name;
        addItem(path, name, cpu.present);
        addAsset(path, cpu.manufacturer, cpu.model, {}, {});
        addLocation(path, cpu.socket);

        DbusInterface interface = addInterface(path, cpuInterface);
        interface->register_property("Socket", cpu.socket);
        interface->register_property("Family", cpu.family);
        interface->register_property("MaxSpeedInMhz", cpu.maxSpeedMHz);
        interface->register_property("CoreCount", cpu.totalCores);
        interface->register_property("ThreadCount", cpu.totalThreads);
        interface->initialize();
    }

    void publishDimm(const ami::inventory::Dimm& dimm, size_t index)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_dimm_" +
                                 sanitizePathComponent(dimm.id, index);
        const std::string name =
            dimm.name.empty() ? (dimm.locator.empty() ? dimm.id : dimm.locator)
                              : dimm.name;
        addItem(path, name, dimm.present);
        addAsset(path, dimm.manufacturer, dimm.formFactor, dimm.partNumber,
                 dimm.serialNumber);
        addLocation(path, dimm.locator);

        DbusInterface interface = addInterface(path, dimmInterface);
        const uint64_t sizeInKiB = dimm.capacityMiB * 1024;
        interface->register_property(
            "MemorySizeInKB",
            static_cast<size_t>(std::min<uint64_t>(
                sizeInKiB, std::numeric_limits<size_t>::max())));
        interface->register_property("MemoryDeviceLocator", dimm.locator);
        interface->register_property("MemoryDataWidth", dimm.dataWidthBits);
        interface->register_property("MemoryTotalWidth", dimm.busWidthBits);
        interface->register_property("MaxMemorySpeedInMhz", dimm.speedMHz);
        interface->register_property("MemoryConfiguredSpeedInMhz",
                                     dimm.speedMHz);
        interface->register_property("Socket", dimm.socket);
        interface->register_property("MemoryController", dimm.memoryController);
        interface->register_property("Channel", dimm.channel);
        interface->register_property("Slot", dimm.slot);
        interface->register_property("MemoryType",
                                     dimmTypeToDbus(dimm.memoryType));
        const std::string formFactor = formFactorToDbus(dimm.formFactor);
        if (!formFactor.empty())
        {
            interface->register_property("FormFactor", formFactor);
        }
        const std::string ecc = eccToDbus(dimm.errorCorrection);
        if (!ecc.empty())
        {
            interface->register_property("ECC", ecc);
        }
        interface->initialize();
    }

    void publishPcie(const ami::inventory::PcieDevice& device, size_t index)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_pcie_" +
                                 sanitizePathComponent(device.id, index);
        const std::string name = device.name.empty() ? device.id : device.name;
        addItem(path, name, device.present);
        addAsset(path, device.manufacturer, device.model, {}, {});
        addLocation(path, device.location);

        DbusInterface interface = addInterface(path, pcieInterface);
        interface->register_property(
            "DeviceType",
            std::string(
                "xyz.openbmc_project.Inventory.Item.PCIeDevice.DeviceTypes.") +
                (device.functions.size() > 1 ? "MultiFunction"
                                             : "SingleFunction"));
        interface->register_property("GenerationInUse",
                                     generationToDbus(device.pcieType));
        interface->register_property("GenerationSupported",
                                     generationToDbus(device.maxPcieType));
        interface->register_property("MaxLanes",
                                     static_cast<size_t>(device.maxLanes));
        interface->register_property("LanesInUse",
                                     static_cast<size_t>(device.lanesInUse));
        for (size_t functionIndex = 0; functionIndex < device.functions.size();
             ++functionIndex)
        {
            const ami::inventory::PcieFunction& function =
                device.functions[functionIndex];
            const std::string prefix =
                "Function" + std::to_string(functionIndex);
            interface->register_property(prefix + "VendorId",
                                         function.vendorId);
            interface->register_property(prefix + "DeviceId",
                                         function.deviceId);
            interface->register_property(prefix + "SubsystemVendorId",
                                         function.subsystemVendorId);
            interface->register_property(prefix + "SubsystemId",
                                         function.subsystemId);
            interface->register_property(prefix + "RevisionId",
                                         function.revisionId);
            interface->register_property(prefix + "ClassCode",
                                         function.classCode);
            interface->register_property(prefix + "DeviceClass",
                                         function.deviceClass);
            interface->register_property(prefix + "FunctionType",
                                         function.functionType);
        }
        interface->initialize();
    }

    void clear()
    {
        for (const DbusInterface& interface : interfaces)
        {
            objectServer.remove_interface(interface);
        }
        interfaces.clear();
    }
};

bool writeAtomically(const fs::path& path, std::string_view contents,
                     std::string& error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "failed to create state directory: " + ec.message();
        return false;
    }

    fs::path temporary = path;
    temporary += ".new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output)
        {
            error = "failed to write temporary state file";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec)
    {
        error = "failed to replace state file: " + ec.message();
        return false;
    }
    return true;
}

std::string readBounded(const fs::path& path, size_t limit)
{
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > limit)
    {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<size_t>(size), '\0');
    if (!input.read(contents.data(), static_cast<std::streamsize>(size)))
    {
        return {};
    }
    return contents;
}

class InventoryService
{
  public:
    explicit InventoryService(
        const std::shared_ptr<sdbusplus::asio::connection>& bus) :
        objectServer(bus), publisher(objectServer)
    {
        control = objectServer.add_interface(std::string(controlPath),
                                             std::string(controlInterface));
        control->register_method("Stage", [this](const std::string& json) {
            return stage(json);
        });
        control->register_method("Commit", [this]() { return commit(); });
        control->register_method("GetCrcs", [this]() { return crcs; });
        control->register_method(
            "SetCrcs", [this](const std::map<std::string, uint32_t>& values) {
                return setCrcs(values);
            });
        control->register_property("Pending", pending);
        control->register_property("LastError", lastError);
        control->initialize();
        restore();
    }

  private:
    sdbusplus::asio::object_server objectServer;
    InventoryPublisher publisher;
    std::shared_ptr<sdbusplus::asio::dbus_interface> control;
    ami::inventory::Inventory committed;
    ami::inventory::Inventory staged;
    std::map<std::string, uint32_t> crcs;
    std::map<std::string, uint32_t> stagedCrcs;
    bool pending = false;
    bool inventoryPending = false;
    bool crcPending = false;
    std::string lastError;

    std::tuple<bool, std::string> stage(const std::string& json)
    {
        ami::inventory::Update update;
        std::string error;
        if (!ami::inventory::parseUpdate(json, update, error))
        {
            setError(error);
            lg2::error("Rejected AMI host inventory: {ERROR}", "ERROR", error);
            return {false, error};
        }
        if (!inventoryPending)
        {
            staged = committed;
        }
        ami::inventory::merge(staged, std::move(update));
        inventoryPending = true;
        updatePending();
        setError({});
        return {true, {}};
    }

    std::tuple<bool, std::string> commit()
    {
        if (!inventoryPending && !crcPending)
        {
            return {true, {}};
        }
        std::string error;
        if (inventoryPending &&
            !writeAtomically(snapshotPath, ami::inventory::serialize(staged),
                             error))
        {
            setError(error);
            lg2::error("Failed to persist AMI host inventory: {ERROR}", "ERROR",
                       error);
            return {false, error};
        }
        if (crcPending)
        {
            const nlohmann::json document = stagedCrcs;
            if (!writeAtomically(crcPath, document.dump(2), error))
            {
                setError(error);
                lg2::error("Failed to persist AMI host CRC state: {ERROR}",
                           "ERROR", error);
                return {false, error};
            }
        }

        if (inventoryPending)
        {
            committed = staged;
            publisher.publish(committed);
            lg2::info(
                "Published AMI host inventory: {CPUS} CPUs, {DIMMS} DIMMs, "
                "{PCIE} PCIe devices",
                "CPUS", committed.cpus.size(), "DIMMS", committed.dimms.size(),
                "PCIE", committed.pcieDevices.size());
        }
        if (crcPending)
        {
            crcs = stagedCrcs;
        }
        inventoryPending = false;
        crcPending = false;
        updatePending();
        setError({});
        return {true, {}};
    }

    std::tuple<bool, std::string> setCrcs(
        const std::map<std::string, uint32_t>& values)
    {
        stagedCrcs = values;
        crcPending = true;
        updatePending();
        setError({});
        return {true, {}};
    }

    void restore()
    {
        const std::string snapshot = readBounded(snapshotPath, maxSnapshotSize);
        if (!snapshot.empty())
        {
            std::string error;
            if (ami::inventory::parseSnapshot(snapshot, committed, error))
            {
                staged = committed;
                publisher.publish(committed);
                lg2::info("Restored persistent AMI host inventory");
            }
            else
            {
                setError(error);
                lg2::error(
                    "Ignoring invalid persistent AMI host inventory: {ERROR}",
                    "ERROR", error);
            }
        }

        const std::string crcState = readBounded(crcPath, 64 * 1024);
        if (!crcState.empty())
        {
            const nlohmann::json document =
                nlohmann::json::parse(crcState, nullptr, false);
            if (document.is_object())
            {
                std::map<std::string, uint32_t> restored;
                bool valid = true;
                for (const auto& [name, value] : document.items())
                {
                    if (!value.is_number_unsigned())
                    {
                        valid = false;
                        break;
                    }
                    const uint64_t number = value.get<uint64_t>();
                    if (number > std::numeric_limits<uint32_t>::max())
                    {
                        valid = false;
                        break;
                    }
                    restored[name] = static_cast<uint32_t>(number);
                }
                if (valid)
                {
                    crcs = std::move(restored);
                    stagedCrcs = crcs;
                }
            }
        }
    }

    void updatePending()
    {
        pending = inventoryPending || crcPending;
        control->set_property("Pending", pending);
    }

    void setError(std::string value)
    {
        lastError = std::move(value);
        control->set_property("LastError", lastError);
    }
};

} // namespace

int main()
{
    boost::asio::io_context io;
    auto bus = std::make_shared<sdbusplus::asio::connection>(io);
    bus->request_name(std::string(serviceName).c_str());
    InventoryService service(bus);
    io.run();
    return 0;
}
