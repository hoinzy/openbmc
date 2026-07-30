// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "ossl_random.hpp"
#include "sessions.hpp"
#include "utility.hpp"
#include "utils/ip_utils.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace redfish
{

constexpr std::string_view amiHostAddress = "169.254.0.18";
constexpr std::string_view amiHostUser = "HostAutoFW";
constexpr std::string_view amiHostPassword = "FwPassword";
constexpr std::string_view amiInventoryService =
    "xyz.openbmc_project.AmiHostInventory";
constexpr std::string_view amiInventoryPath =
    "/xyz/openbmc_project/inventory/ami_host";
constexpr std::string_view amiInventoryInterface =
    "xyz.openbmc_project.AmiHostInventory";
constexpr size_t amiInventoryLimit = 2 * 1024 * 1024;
constexpr std::string_view amiBiosStaticDirectory =
    "/var/lib/ami-host-inventory/bios-static";
constexpr std::string_view amiBiosDataDirectory =
    "/var/lib/ami-host-inventory/bios";

inline std::optional<std::string> getAmiMultipartFilename(
    std::string_view disposition);

struct AmiExtensionInfo
{
    std::string_view id;
    std::string_view md5;
    std::string_view name;
    std::string_view directory;
};

constexpr std::array<AmiExtensionInfo, 2> amiExtensions = {{
    {
        "34E46539-1213-4208-9AB6-2D1C21A35523",
        "24e5614de3ead58517b9a1f001f272a8",
        "AMI Inventory Extension",
        "ami_inventory",
    },
    {
        "24C5E8D6-7D92-4E54-916E-FEE44013F13F",
        "f33b77b217dd5fc1deae86d64c5b1e28",
        "AMI BIOS Default Data Extension",
        "ami_bios_default",
    },
}};

inline bool isAmiHost(const crow::Request& req)
{
    return redfish::ip_util::toString(req.ipAddress) == amiHostAddress;
}

inline bool checkAmiHostCredentials(const crow::Request& req)
{
    if (!isAmiHost(req))
    {
        return false;
    }

    std::string_view token = req.getHeaderValue("X-Auth-Token");
    if (!token.empty())
    {
        std::shared_ptr<persistent_data::UserSession> session =
            persistent_data::SessionStore::getInstance().loginSessionByToken(
                token);
        return session != nullptr && session->username == amiHostUser &&
               session->clientIp == amiHostAddress;
    }

    std::string_view authorization =
        req.getHeaderValue(boost::beast::http::field::authorization);
    if (authorization.empty())
    {
        // AMI RedfishHi authentication mode zero explicitly uses AuthNone.
        // The fixed source address on the point-to-point USB link remains
        // mandatory, so this does not expose an unauthenticated LAN route.
        return true;
    }
    if (!authorization.starts_with("Basic "))
    {
        return false;
    }
    std::string decoded;
    if (!crow::utility::base64Decode(authorization.substr(6), decoded))
    {
        return false;
    }
    const std::string expected =
        std::string(amiHostUser) + ":" + std::string(amiHostPassword);
    return bmcweb::constantTimeStringCompare(decoded, expected);
}

inline void rejectAmiHostRequest(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    BMCWEB_LOG_WARNING("Rejected AMI inventory request from {}",
                       redfish::ip_util::toString(req.ipAddress));
    messages::resourceAtUriUnauthorized(
        asyncResp->res, req.url(),
        "AMI host inventory is restricted to the USB host interface");
}

inline void amiBadRequest(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          std::string_view message)
{
    asyncResp->res.result(boost::beast::http::status::bad_request);
    asyncResp->res.jsonValue["error"]["code"] = "Base.1.16.GeneralError";
    asyncResp->res.jsonValue["error"]["message"] = message;
}

inline bool handleAmiHostSessionPost(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!isAmiHost(req))
    {
        return false;
    }
    nlohmann::json request = nlohmann::json::parse(req.body(), nullptr, false);
    if (!request.is_object())
    {
        return false;
    }
    const auto username = request.find("UserName");
    const auto password = request.find("Password");
    if (username == request.end() || password == request.end() ||
        !username->is_string() || !password->is_string() ||
        username->get_ref<const std::string&>() != amiHostUser ||
        password->get_ref<const std::string&>() != amiHostPassword)
    {
        return false;
    }

    std::shared_ptr<persistent_data::UserSession> session =
        persistent_data::SessionStore::getInstance().generateUserSession(
            std::string(amiHostUser), req.ipAddress, std::nullopt,
            persistent_data::SessionType::Session, false);
    if (session == nullptr)
    {
        messages::internalError(asyncResp->res);
        return true;
    }
    asyncResp->res.addHeader("X-Auth-Token", session->sessionToken);
    asyncResp->res.addHeader(
        "Location", "/redfish/v1/SessionService/Sessions/" + session->uniqueId);
    asyncResp->res.result(boost::beast::http::status::created);
    asyncResp->res.jsonValue["Id"] = session->uniqueId;
    asyncResp->res.jsonValue["UserName"] = amiHostUser;
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/SessionService/Sessions/" + session->uniqueId;
    return true;
}

inline std::optional<std::string> extractAmiInventory(crow::Request& req)
{
    for (FormPart& part : req.multipart())
    {
        auto disposition =
            part.fields.find(boost::beast::http::field::content_disposition);
        if (disposition == part.fields.end())
        {
            continue;
        }
        const std::string_view value = disposition->value();
        if (value.find("name=\"static_file\"") == std::string_view::npos ||
            value.find("filename=\"inventory.json\"") == std::string_view::npos)
        {
            continue;
        }
        if (part.content.empty() || part.content.size() > amiInventoryLimit)
        {
            return std::nullopt;
        }
        return std::move(part.content);
    }
    return std::nullopt;
}

inline void handleAmiInventoryGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] = "/redfish/v1/Oem/Ami/InventoryData";
    asyncResp->res.jsonValue["Id"] = "InventoryData";
    asyncResp->res.jsonValue["Name"] = "AMI Host Inventory Data";
    asyncResp->res.jsonValue["BootComplete"] = false;
    // RfInventory consumes the existing System, Chassis, and Storage resources
    // before constructing its replacement inventory.  Keep these links in the
    // compatibility response even when no host inventory has been committed.
    nlohmann::json& system = asyncResp->res.jsonValue["System"];
    system["@odata.id"] = "/redfish/v1/Systems/Self";
    system["Id"] = "Self";
    system["Storage"] = {
        {"@odata.id", "/redfish/v1/Systems/Self/Storage"}};
    asyncResp->res.jsonValue["Chassis"] = {
        {"@odata.id", "/redfish/v1/Chassis/Self"}, {"Id", "Self"}};
    asyncResp->res.jsonValue["Storage"] = {
        {"@odata.id", "/redfish/v1/Systems/Self/Storage"},
        {"Members", nlohmann::json::array()},
        {"Members@odata.count", 0}};
    nlohmann::json dynamicExtensions = nlohmann::json::array();
    for (const AmiExtensionInfo& extension : amiExtensions)
    {
        dynamicExtensions.push_back(
            {{"Id", extension.id}, {"Md5Checksum", extension.md5}});
    }
    asyncResp->res.jsonValue["DRE"] = std::move(dynamicExtensions);
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec,
                    const std::map<std::string, uint32_t>& crcs) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory GetCrcs failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            nlohmann::json groupCrcList = nlohmann::json::array();
            for (std::string_view group : {"DIMM", "CPU", "PCIE"})
            {
                auto crc = crcs.find(std::string(group));
                const uint32_t value = crc == crcs.end() ? 0 : crc->second;
                groupCrcList.push_back({{group, value}});
            }
            asyncResp->res.jsonValue["System"]["Oem"]["Ami"]["Bios"]
                                        ["Inventory"]["Crc"]["GroupCrcList"] =
                groupCrcList;
            asyncResp->res.jsonValue["GroupCrcList"] =
                std::move(groupCrcList);
        },
        std::string(amiInventoryService), std::string(amiInventoryPath),
        std::string(amiInventoryInterface), "GetCrcs");
}

inline void handleAmiInventoryPost(
    const crow::Request& reqIn,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(reqIn))
    {
        rejectAmiHostRequest(reqIn, asyncResp);
        return;
    }
    crow::Request& req = const_cast<crow::Request&>(reqIn);
    std::optional<std::string> inventory = extractAmiInventory(req);
    if (!inventory)
    {
        amiBadRequest(asyncResp,
                      "Expected static_file inventory.json multipart data");
        return;
    }
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec,
                    const std::tuple<bool, std::string>& result) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory Stage failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            const auto& [accepted, error] = result;
            if (!accepted)
            {
                amiBadRequest(asyncResp, error);
                return;
            }
            asyncResp->res.result(boost::beast::http::status::no_content);
        },
        std::string(amiInventoryService), std::string(amiInventoryPath),
        std::string(amiInventoryInterface), "Stage", *inventory);
}

inline void handleAmiInventoryPatch(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    nlohmann::json request = nlohmann::json::parse(req.body(), nullptr, false);
    if (!request.is_object() || request.value("BootComplete", false) != true)
    {
        amiBadRequest(asyncResp, "BootComplete must be true");
        return;
    }
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec,
                    const std::tuple<bool, std::string>& result) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory Commit failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            const auto& [committed, error] = result;
            if (!committed)
            {
                amiBadRequest(asyncResp, error);
                return;
            }
            asyncResp->res.result(boost::beast::http::status::no_content);
        },
        std::string(amiInventoryService), std::string(amiInventoryPath),
        std::string(amiInventoryInterface), "Commit");
}

inline void handleAmiCrcGet(const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec,
                    const std::map<std::string, uint32_t>& crcs) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory GetCrcs failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            nlohmann::json groupCrcList = nlohmann::json::array();
            for (std::string_view group : {"DIMM", "CPU", "PCIE"})
            {
                auto crc = crcs.find(std::string(group));
                if (crc != crcs.end())
                {
                    groupCrcList.push_back({{group, crc->second}});
                }
            }
            asyncResp->res.jsonValue["GroupCrcList"] = std::move(groupCrcList);
        },
        std::string(amiInventoryService), std::string(amiInventoryPath),
        std::string(amiInventoryInterface), "GetCrcs");
}

inline std::optional<uint32_t> getAmiCrc(const nlohmann::json& value)
{
    if (value.is_number_unsigned())
    {
        const uint64_t number = value.get<uint64_t>();
        if (number <= std::numeric_limits<uint32_t>::max())
        {
            return static_cast<uint32_t>(number);
        }
    }
    else if (value.is_number_integer())
    {
        const int64_t number = value.get<int64_t>();
        if (number >= 0 && static_cast<uint64_t>(number) <=
                               std::numeric_limits<uint32_t>::max())
        {
            return static_cast<uint32_t>(number);
        }
    }
    return std::nullopt;
}

inline bool isAmiCrcGroup(std::string_view name)
{
    return name == "CPU" || name == "DIMM" || name == "PCIE";
}

inline std::optional<std::map<std::string, uint32_t>> parseAmiCrcs(
    const crow::Request& req)
{
    nlohmann::json request = nlohmann::json::parse(req.body(), nullptr, false);
    if (!request.is_object())
    {
        return std::nullopt;
    }
    auto list = request.find("GroupCrcList");
    if (list == request.end())
    {
        return std::nullopt;
    }
    std::map<std::string, uint32_t> crcs;
    if (list->is_object())
    {
        for (const auto& [name, value] : list->items())
        {
            if (!isAmiCrcGroup(name))
            {
                return std::nullopt;
            }
            std::optional<uint32_t> crc = getAmiCrc(value);
            if (!crc)
            {
                return std::nullopt;
            }
            crcs[name] = *crc;
        }
        return crcs;
    }
    if (!list->is_array())
    {
        return std::nullopt;
    }
    for (const nlohmann::json& entry : *list)
    {
        if (!entry.is_object() || entry.size() != 1)
        {
            return std::nullopt;
        }
        const auto value = entry.begin();
        if (!isAmiCrcGroup(value.key()) || crcs.contains(value.key()))
        {
            return std::nullopt;
        }
        std::optional<uint32_t> crc = getAmiCrc(value.value());
        if (!crc)
        {
            return std::nullopt;
        }
        crcs[value.key()] = *crc;
    }
    return crcs;
}

inline void handleAmiCrcPost(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    std::optional<std::map<std::string, uint32_t>> crcs = parseAmiCrcs(req);
    if (!crcs)
    {
        amiBadRequest(asyncResp, "GroupCrcList is invalid");
        return;
    }
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec,
                    const std::tuple<bool, std::string>& result) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory SetCrcs failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            const auto& [stored, error] = result;
            if (!stored)
            {
                amiBadRequest(asyncResp, error);
                return;
            }
            asyncResp->res.result(boost::beast::http::status::no_content);
        },
        std::string(amiInventoryService), std::string(amiInventoryPath),
        std::string(amiInventoryInterface), "SetCrcs", *crcs);
}

inline void handleAmiExtensionCollectionGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/DynamicExtension/RedfishExtensions";
    asyncResp->res.jsonValue["@odata.type"] =
        "#DynamicExtensionCollection.DynamicExtensionCollection";
    asyncResp->res.jsonValue["Name"] = "Redfish Extension Collection";
    nlohmann::json members = nlohmann::json::array();
    for (const AmiExtensionInfo& extension : amiExtensions)
    {
        members.push_back(
            {{"@odata.id",
              "/redfish/v1/DynamicExtension/RedfishExtensions/" +
                  std::string(extension.id)}});
    }
    asyncResp->res.jsonValue["Members"] = std::move(members);
    asyncResp->res.jsonValue["Members@odata.count"] = amiExtensions.size();
}

inline const AmiExtensionInfo* getAmiExtension(std::string_view id)
{
    for (const AmiExtensionInfo& extension : amiExtensions)
    {
        if (extension.id == id)
        {
            return &extension;
        }
    }
    return nullptr;
}

inline void populateAmiExtension(
    const AmiExtensionInfo& extension,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    const std::string path =
        "/redfish/v1/DynamicExtension/RedfishExtensions/" +
        std::string(extension.id);
    asyncResp->res.jsonValue["@odata.id"] = path;
    asyncResp->res.jsonValue["@odata.type"] =
        "#DynamicExtension.v1_0_0.DynamicExtension";
    asyncResp->res.jsonValue["Id"] = extension.id;
    asyncResp->res.jsonValue["Name"] = extension.name;
    asyncResp->res.jsonValue["Description"] =
        "Native OpenBMC compatibility for AMI firmware services";
    asyncResp->res.jsonValue["DirectoryName"] = extension.directory;
    asyncResp->res.jsonValue["Md5Checksum"] = extension.md5;
    asyncResp->res.jsonValue["Running"] = true;
    asyncResp->res.jsonValue["PendingDeletion"] = false;
}

inline void handleAmiExtensionGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& id)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    const AmiExtensionInfo* extension = getAmiExtension(id);
    if (extension == nullptr)
    {
        asyncResp->res.result(boost::beast::http::status::not_found);
        return;
    }
    populateAmiExtension(*extension, asyncResp);
}

inline std::optional<std::string> extractAmiExtensionId(crow::Request& req)
{
    for (FormPart& part : req.multipart())
    {
        auto disposition =
            part.fields.find(boost::beast::http::field::content_disposition);
        if (disposition == part.fields.end())
        {
            continue;
        }
        const std::string_view value = disposition->value();
        std::optional<std::string> filename =
            getAmiMultipartFilename(value);
        if (!filename || filename->size() > 128 ||
            filename->find('/') != std::string::npos ||
            filename->find('\\') != std::string::npos)
        {
            continue;
        }
        if (part.content.empty() || part.content.size() > amiInventoryLimit)
        {
            return std::nullopt;
        }
        return filename;
    }
    return std::nullopt;
}

inline void handleAmiExtensionPost(
    const crow::Request& reqIn,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(reqIn))
    {
        rejectAmiHostRequest(reqIn, asyncResp);
        return;
    }
    crow::Request& req = const_cast<crow::Request&>(reqIn);
    std::optional<std::string> id = extractAmiExtensionId(req);
    const AmiExtensionInfo* extension =
        id ? getAmiExtension(*id) : nullptr;
    if (extension == nullptr)
    {
        amiBadRequest(asyncResp,
                      "Expected a compressed dynamic-extension archive");
        return;
    }

    // The vendor BMC extracts and executes this archive.  This implementation
    // deliberately discards it because the inventory receiver is native
    // OpenBMC code.  Report the matching extension identity as created so the
    // firmware can continue without executing vendor-supplied Lua.
    asyncResp->res.addHeader(
        "Location",
        "/redfish/v1/DynamicExtension/RedfishExtensions/" +
            std::string(extension->id));
    asyncResp->res.result(boost::beast::http::status::created);
    populateAmiExtension(*extension, asyncResp);
}

inline bool isAmiBiosStaticFilename(std::string_view filename)
{
    return filename == "ComboButton.png" || filename == "Index.css" ||
           filename == "Favicon.ico" || filename == "Index.html" ||
           filename == "Index.js" || filename == "RbLogo.png" ||
           filename == "SetupData.xml";
}

inline std::optional<std::string> getAmiMultipartFilename(
    std::string_view disposition)
{
    constexpr std::string_view prefix = "filename=\"";
    const size_t start = disposition.find(prefix);
    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }
    const size_t valueStart = start + prefix.size();
    const size_t end = disposition.find('"', valueStart);
    if (end == std::string_view::npos || end == valueStart)
    {
        return std::nullopt;
    }
    return std::string(disposition.substr(valueStart, end - valueStart));
}

inline bool persistAmiFile(std::string_view directoryName,
                           std::string_view filename,
                           std::string_view content)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path directory{directoryName};
    fs::create_directories(directory, ec);
    if (ec)
    {
        BMCWEB_LOG_ERROR("Failed to create AMI BIOS static directory: {}",
                         ec.message());
        return false;
    }

    const fs::path output = directory / filename;
    fs::path temporary = output;
    temporary += ".new";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            BMCWEB_LOG_ERROR("Failed to open AMI BIOS static file {}",
                             temporary.string());
            return false;
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file)
        {
            BMCWEB_LOG_ERROR("Failed to write AMI BIOS static file {}",
                             temporary.string());
            return false;
        }
    }
    fs::rename(temporary, output, ec);
    if (ec)
    {
        BMCWEB_LOG_ERROR("Failed to install AMI BIOS static file {}: {}",
                         output.string(), ec.message());
        std::error_code removeEc;
        fs::remove(temporary, removeEc);
        return false;
    }
    return true;
}

inline std::optional<std::string> loadAmiFile(std::string_view directoryName,
                                              std::string_view filename)
{
    namespace fs = std::filesystem;
    const fs::path input = fs::path(directoryName) / filename;
    std::error_code ec;
    const uintmax_t fileSize = fs::file_size(input, ec);
    if (ec || fileSize > amiInventoryLimit)
    {
        return std::nullopt;
    }

    std::ifstream file(input, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::string content(static_cast<size_t>(fileSize), '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file)
    {
        return std::nullopt;
    }
    return content;
}

inline void handleAmiBiosStaticPost(
    const crow::Request& reqIn,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(reqIn))
    {
        rejectAmiHostRequest(reqIn, asyncResp);
        return;
    }

    crow::Request& req = const_cast<crow::Request&>(reqIn);
    for (FormPart& part : req.multipart())
    {
        auto disposition =
            part.fields.find(boost::beast::http::field::content_disposition);
        if (disposition == part.fields.end())
        {
            continue;
        }
        const std::string_view value = disposition->value();
        if (value.find("name=\"static_file\"") == std::string_view::npos)
        {
            continue;
        }
        std::optional<std::string> filename =
            getAmiMultipartFilename(value);
        if (!filename || !isAmiBiosStaticFilename(*filename) ||
            part.content.empty() || part.content.size() > amiInventoryLimit)
        {
            continue;
        }
        if (!persistAmiFile(amiBiosStaticDirectory, *filename, part.content))
        {
            messages::internalError(asyncResp->res);
            return;
        }

        const std::string location =
            "/redfish/v1/BiosStaticFiles/" + *filename;
        asyncResp->res.addHeader("Location", location);
        asyncResp->res.result(boost::beast::http::status::created);
        asyncResp->res.jsonValue["@odata.id"] = location;
        asyncResp->res.jsonValue["Id"] = *filename;
        asyncResp->res.jsonValue["Name"] = "AMI BIOS static file";
        asyncResp->res.jsonValue["Status"] = "Completed";
        return;
    }

    amiBadRequest(asyncResp, "Expected a supported AMI BIOS static file");
}

inline void handleAmiBiosStaticGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& filename)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    if (!isAmiBiosStaticFilename(filename))
    {
        asyncResp->res.result(boost::beast::http::status::not_found);
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/BiosStaticFiles/" + filename;
    asyncResp->res.jsonValue["Id"] = filename;
    asyncResp->res.jsonValue["Name"] = "AMI BIOS static file";
    asyncResp->res.jsonValue["Status"] = "Completed";
}

inline bool isAmiBiosRegistryFilename(std::string_view filename)
{
    return filename.starts_with("BiosAttributeRegistry") &&
           filename.ends_with(".json") &&
           filename.find('/') == std::string_view::npos &&
           filename.find('\\') == std::string_view::npos;
}

inline std::optional<nlohmann::json> parseAmiJsonBody(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (req.body().empty() || req.body().size() > amiInventoryLimit)
    {
        amiBadRequest(asyncResp, "Expected a bounded JSON request body");
        return std::nullopt;
    }
    nlohmann::json request = nlohmann::json::parse(req.body(), nullptr, false);
    if (!request.is_object())
    {
        amiBadRequest(asyncResp, "Expected a JSON object");
        return std::nullopt;
    }
    return request;
}

inline void handleAmiBiosRegistryPost(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& filename)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    if (!isAmiBiosRegistryFilename(filename) ||
        !parseAmiJsonBody(req, asyncResp))
    {
        return;
    }
    if (!persistAmiFile(amiBiosDataDirectory, filename, req.body()))
    {
        messages::internalError(asyncResp->res);
        return;
    }

    const std::string location = "/redfish/v1/Registries/" + filename;
    asyncResp->res.addHeader("Location", location);
    asyncResp->res.result(boost::beast::http::status::created);
    asyncResp->res.jsonValue["@odata.id"] = location;
    asyncResp->res.jsonValue["Id"] = filename;
    asyncResp->res.jsonValue["Name"] = "AMI BIOS attribute registry";
}

inline nlohmann::json getAmiDefaultSd()
{
    std::optional<std::string> stored =
        loadAmiFile(amiBiosDataDirectory, "default-sd.json");
    if (stored)
    {
        nlohmann::json data = nlohmann::json::parse(*stored, nullptr, false);
        if (data.is_object() && data.contains("DefaultData"))
        {
            return data;
        }
    }
    return {{"DefaultData", {{{"FailedMapIdList", ""}}}}};
}

inline void handleAmiDefaultSdGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    asyncResp->res.jsonValue = getAmiDefaultSd();
}

inline void handleAmiDefaultSdPost(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    std::optional<nlohmann::json> request = parseAmiJsonBody(req, asyncResp);
    if (!request || !request->contains("DefaultData") ||
        !(*request)["DefaultData"].is_array())
    {
        if (request)
        {
            amiBadRequest(asyncResp, "Expected a DefaultData array");
        }
        return;
    }
    if (!persistAmiFile(amiBiosDataDirectory, "default-sd.json", req.body()))
    {
        messages::internalError(asyncResp->res);
        return;
    }
    asyncResp->res.jsonValue = std::move(*request);
}

inline nlohmann::json getAmiSystemBios()
{
    std::optional<std::string> stored =
        loadAmiFile(amiBiosDataDirectory, "current-bios.json");
    if (stored)
    {
        nlohmann::json data = nlohmann::json::parse(*stored, nullptr, false);
        if (data.is_object())
        {
            return data;
        }
    }
    return {
        {"@odata.id", "/redfish/v1/Systems/Self/Bios"},
        {"Id", "Bios"},
        {"Name", "AMI Host BIOS Configuration"},
        {"Attributes", nlohmann::json::object()},
    };
}

inline void handleAmiSystemBiosGet(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    asyncResp->res.jsonValue = getAmiSystemBios();
}

inline void handleAmiSystemBiosPost(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!checkAmiHostCredentials(req))
    {
        rejectAmiHostRequest(req, asyncResp);
        return;
    }
    std::optional<nlohmann::json> request = parseAmiJsonBody(req, asyncResp);
    if (!request)
    {
        return;
    }
    if (!persistAmiFile(amiBiosDataDirectory, "current-bios.json", req.body()))
    {
        messages::internalError(asyncResp->res);
        return;
    }
    asyncResp->res.jsonValue = std::move(*request);
}

inline void requestRoutesAmiHostInventory(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Oem/Ami/InventoryData/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiInventoryGet);
    BMCWEB_ROUTE(app, "/redfish/v1/Oem/Ami/InventoryData/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiInventoryPost);
    BMCWEB_ROUTE(app, "/redfish/v1/Oem/Ami/InventoryData/")
        .privileges({})
        .methods(boost::beast::http::verb::patch)(handleAmiInventoryPatch);
    BMCWEB_ROUTE(app, "/redfish/v1/oem/ami/inventory/crc/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiCrcGet);
    BMCWEB_ROUTE(app, "/redfish/v1/oem/ami/inventory/crc/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiCrcPost);
    BMCWEB_ROUTE(
        app, "/redfish/v1/DynamicExtension/RedfishExtensions/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(
            handleAmiExtensionCollectionGet);
    BMCWEB_ROUTE(
        app, "/redfish/v1/DynamicExtension/RedfishExtensions/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiExtensionPost);
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/DynamicExtension/RedfishExtensions/<str>/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiExtensionGet);
    BMCWEB_ROUTE(app, "/redfish/v1/BiosStaticFiles/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiBiosStaticPost);
    BMCWEB_ROUTE(app, "/redfish/v1/BiosStaticFiles/<str>/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiBiosStaticGet);
    BMCWEB_ROUTE(app, "/redfish/v1/Registries/<str>/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiBiosRegistryPost);
    BMCWEB_ROUTE(app, "/redfish/v1/bios/defaultSD/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiDefaultSdGet);
    BMCWEB_ROUTE(app, "/redfish/v1/bios/defaultSD/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiDefaultSdPost);
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/Self/Bios/")
        .privileges({})
        .methods(boost::beast::http::verb::get)(handleAmiSystemBiosGet);
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/Self/Bios/")
        .privileges({})
        .methods(boost::beast::http::verb::post)(handleAmiSystemBiosPost);
}

} // namespace redfish
