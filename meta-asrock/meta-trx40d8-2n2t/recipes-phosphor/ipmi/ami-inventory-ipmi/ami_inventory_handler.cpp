/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_inventory.hpp"

#include <ipmid/api.hpp>
#include <ipmid/handler.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;

constexpr uint8_t amiInventoryCommand = 0x5a;
constexpr size_t selectorSize = 3;
constexpr size_t maxPersistedPayloadSize = 64 * 1024;
constexpr std::string_view inventoryRoot =
    "/xyz/openbmc_project/inventory/system/chassis/motherboard";
constexpr std::string_view itemInterface = "xyz.openbmc_project.Inventory.Item";
constexpr std::string_view assetInterface =
    "xyz.openbmc_project.Inventory.Decorator.Asset";
constexpr std::string_view locationInterface =
    "xyz.openbmc_project.Inventory.Decorator.LocationCode";
constexpr std::string_view cpuInterface = "xyz.openbmc_project.Inventory.Item.Cpu";
constexpr std::string_view dimmInterface =
    "xyz.openbmc_project.Inventory.Item.Dimm";
constexpr std::string_view pcieDeviceInterface =
    "xyz.openbmc_project.Inventory.Item.PCIeDevice";
const fs::path persistedPayload = "/var/lib/ami-inventory/inventory.bin";

std::string hexValue(uint32_t value, unsigned int width)
{
    char buffer[11]{};
    std::snprintf(buffer, sizeof(buffer), "0x%0*x", width, value);
    return buffer;
}

std::string dimmTypeToDbus(const std::string& type)
{
    constexpr std::string_view prefix =
        "xyz.openbmc_project.Inventory.Item.Dimm.DeviceType.";
    if (type == "DDR4")
    {
        return std::string(prefix) + "DDR4";
    }
    if (type == "DDR5")
    {
        return std::string(prefix) + "DDR5";
    }
    if (type == "DDR3")
    {
        return std::string(prefix) + "DDR3";
    }
    if (type == "DDR2")
    {
        return std::string(prefix) + "DDR2";
    }
    if (type == "DDR")
    {
        return std::string(prefix) + "DDR";
    }
    return {};
}

class InventoryPublisher
{
  public:
    void publish(const std::shared_ptr<sdbusplus::asio::connection>& bus,
                 const ami::inventory::Inventory& inventory)
    {
        if (!objectServer)
        {
            // phosphor-host-ipmid already owns its object manager.  This
            // provider only adds inventory interfaces to that same service.
            objectServer = std::make_unique<sdbusplus::asio::object_server>(
                bus, true);
        }

        clear();
        for (const auto& cpu : inventory.cpus)
        {
            publishCpu(cpu);
        }
        for (const auto& dimm : inventory.dimms)
        {
            publishDimm(dimm);
        }
        for (const auto& pci : inventory.pciDevices)
        {
            publishPci(pci);
        }
    }

    void restore()
    {
        std::error_code ec;
        const auto fileSize = fs::file_size(persistedPayload, ec);
        if (ec || fileSize == 0)
        {
            return;
        }
        if (fileSize > maxPersistedPayloadSize)
        {
            lg2::error("Ignoring oversized persisted AMI inventory payload");
            return;
        }

        std::ifstream input(persistedPayload, std::ios::binary);
        std::vector<uint8_t> payload(
            static_cast<size_t>(fileSize));
        if (!input.read(reinterpret_cast<char*>(payload.data()),
                        static_cast<std::streamsize>(payload.size())))
        {
            lg2::error("Failed to read persisted AMI inventory payload");
            return;
        }

        ami::inventory::Inventory inventory;
        std::string error;
        if (!ami::inventory::parse(payload, inventory, error))
        {
            lg2::error("Ignoring invalid persisted AMI inventory payload: {ERROR}",
                       "ERROR", error);
            return;
        }
        publish(ipmi::getSdBus(), inventory);
        lg2::info("Restored AMI host inventory from BMC persistent storage");
    }

    void persist(const std::vector<uint8_t>& payload)
    {
        std::error_code ec;
        fs::create_directories(persistedPayload.parent_path(), ec);
        if (ec)
        {
            lg2::error("Failed to create AMI inventory state directory: {ERROR}",
                       "ERROR", ec.message());
            return;
        }

        const fs::path temporary = persistedPayload.string() + ".new";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(payload.data()),
                         static_cast<std::streamsize>(payload.size()));
            if (!output)
            {
                lg2::error("Failed to write AMI inventory persistent state");
                return;
            }
        }
        fs::rename(temporary, persistedPayload, ec);
        if (ec)
        {
            lg2::error("Failed to replace AMI inventory persistent state: {ERROR}",
                       "ERROR", ec.message());
        }
    }

  private:
    using DbusInterface = std::shared_ptr<sdbusplus::asio::dbus_interface>;

    std::unique_ptr<sdbusplus::asio::object_server> objectServer;
    std::vector<DbusInterface> interfaces;

    DbusInterface addInterface(const std::string& path,
                               std::string_view interface)
    {
        auto result = objectServer->add_interface(path, std::string(interface));
        interfaces.emplace_back(result);
        return result;
    }

    void addItem(const std::string& path, const std::string& name)
    {
        auto item = addInterface(path, itemInterface);
        item->register_property("PrettyName", name);
        item->register_property("Present", true);
        item->initialize();
    }

    void addAsset(const std::string& path, const std::string& manufacturer,
                  const std::string& model, const std::string& partNumber,
                  const std::string& serialNumber)
    {
        auto asset = addInterface(path, assetInterface);
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
        auto decorator = addInterface(path, locationInterface);
        decorator->register_property("LocationCode", location);
        decorator->initialize();
    }

    void publishCpu(const ami::inventory::Cpu& cpu)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_cpu_" +
                                 std::to_string(cpu.recordId);
        addItem(path, cpu.brand.empty() ? "CPU " + std::to_string(cpu.index)
                                        : cpu.brand);
        addAsset(path, cpu.vendor, cpu.brand, {}, {});
        addLocation(path, cpu.location);

        auto interface = addInterface(path, cpuInterface);
        interface->register_property("Socket", cpu.location);
        interface->register_property("Family", cpu.family);
        interface->register_property("MaxSpeedInMhz", cpu.maxFrequencyMhz);
        interface->register_property("CoreCount", cpu.coreCount);
        interface->register_property("ThreadCount", cpu.threadCount);
        interface->initialize();
    }

    void publishDimm(const ami::inventory::Dimm& dimm)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_dimm_" +
                                 std::to_string(dimm.recordId);
        const std::string locator = dimm.location.empty()
                                        ? "DIMM " + std::to_string(dimm.index)
                                        : dimm.location;
        addItem(path, locator);
        addAsset(path, dimm.manufacturer, dimm.moduleType, dimm.partNumber,
                 dimm.serialNumber);
        addLocation(path, dimm.location);

        auto interface = addInterface(path, dimmInterface);
        interface->register_property(
            "MemorySizeInKB", static_cast<size_t>(dimm.sizeMiB) * 1024);
        interface->register_property("MemoryDeviceLocator", locator);
        interface->register_property("MaxMemorySpeedInMhz", dimm.speedMhz);
        interface->register_property("MemoryConfiguredSpeedInMhz",
                                     dimm.speedMhz);
        interface->register_property("Socket", dimm.socket);
        interface->register_property("Channel", dimm.channel);
        interface->register_property("MemoryController", dimm.memoryController);
        interface->register_property("Slot", dimm.slot);
        const std::string memoryType = dimmTypeToDbus(dimm.dramType);
        if (!memoryType.empty())
        {
            interface->register_property("MemoryType", memoryType);
        }
        interface->initialize();
    }

    void publishPci(const ami::inventory::Pci& pci)
    {
        const std::string path = std::string(inventoryRoot) + "/ami_pcie_" +
                                 std::to_string(pci.recordId);
        const std::string bdf = hexValue(pci.segment, 2) + ":" +
                                hexValue(pci.bus, 2) + ":" +
                                hexValue(pci.deviceFunction >> 3, 2) + "." +
                                std::to_string(pci.deviceFunction & 0x07);
        addItem(path, "PCIe " + bdf);
        addAsset(path, {}, hexValue(pci.vendorId, 4) + ":" +
                                hexValue(pci.deviceId, 4),
                 {}, {});
        addLocation(path, pci.location);

        auto interface = addInterface(path, pcieDeviceInterface);
        interface->register_property("Function0VendorId",
                                     hexValue(pci.vendorId, 4));
        interface->register_property("Function0DeviceId",
                                     hexValue(pci.deviceId, 4));
        interface->register_property("Function0SubsystemVendorId",
                                     hexValue(pci.subsystemVendorId, 4));
        interface->register_property("Function0SubsystemId",
                                     hexValue(pci.subsystemId, 4));
        interface->register_property("Function0RevisionId",
                                     hexValue(pci.revisionId, 2));
        interface->register_property(
            "Function0ClassCode",
            hexValue(pci.classCode, 2) + hexValue(pci.subclassCode, 2) +
                hexValue(pci.programmingInterface, 2));
        interface->initialize();
    }

    void clear()
    {
        for (const auto& interface : interfaces)
        {
            objectServer->remove_interface(interface);
        }
        interfaces.clear();
    }
};

InventoryPublisher publisher;

ipmi::RspType<> setAmiInventory(ipmi::Context::ptr context,
                                std::vector<uint8_t> request)
{
    if (context->channel != ipmi::channelSystemIface)
    {
        return ipmi::response(ipmi::ccInsufficientPrivilege);
    }
    if (request.size() < selectorSize)
    {
        return ipmi::responseReqDataLenInvalid();
    }
    if (request[0] != 0 || request[1] != 0 || request[2] != 0)
    {
        // The firmware's complete inventory update uses selector 0.  Other
        // vendor selectors mutate auxiliary state and are intentionally not
        // exposed by this compatibility provider.
        return ipmi::responseIllegalCommand();
    }

    std::vector<uint8_t> payload(request.begin() + selectorSize, request.end());
    ami::inventory::Inventory inventory;
    std::string error;
    if (!ami::inventory::parse(payload, inventory, error))
    {
        lg2::error("Rejected AMI host inventory update: {ERROR}", "ERROR",
                   error);
        return ipmi::responseInvalidFieldRequest();
    }

    publisher.publish(context->bus, inventory);
    publisher.persist(payload);
    lg2::info("Published AMI host inventory: {CPUS} CPUs, {DIMMS} DIMMs, {PCI} PCI devices",
              "CPUS", inventory.cpus.size(), "DIMMS", inventory.dimms.size(),
              "PCI", inventory.pciDevices.size());
    return ipmi::responseSuccess();
}

void registerAmiInventoryFunctions() __attribute__((constructor));
void registerAmiInventoryFunctions()
{
    ipmi::registerHandler(ipmi::prioOemBase, ipmi::netFnOemTwo,
                          amiInventoryCommand, ipmi::Privilege::Admin,
                          setAmiInventory);
    ipmi::post_work([]() { publisher.restore(); });
}

} // namespace
