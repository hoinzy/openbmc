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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
        [asyncResp](const boost::system::error_code& ec, bool accepted,
                    const std::string& error) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory Stage failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
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
        [asyncResp](const boost::system::error_code& ec, bool committed,
                    const std::string& error) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory Commit failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
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
        [asyncResp](const boost::system::error_code& ec, bool stored,
                    const std::string& error) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("AMI inventory SetCrcs failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
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
}

} // namespace redfish
