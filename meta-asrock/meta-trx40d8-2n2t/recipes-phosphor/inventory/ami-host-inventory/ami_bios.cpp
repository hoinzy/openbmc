/*
 * SPDX-FileCopyrightText: Copyright OpenBMC Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ami_bios.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ami::bios
{
namespace
{

using Json = nlohmann::json;

constexpr size_t maxAttributes = 1024;
constexpr size_t maxAttributeName = 128;
constexpr int64_t defaultMaxStringLength = 8192;

std::optional<int64_t> getInteger(const Json& value)
{
    if (value.is_number_integer())
    {
        return value.get<int64_t>();
    }
    if (!value.is_number_unsigned())
    {
        return std::nullopt;
    }
    const uint64_t number = value.get<uint64_t>();
    if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        return std::nullopt;
    }
    return static_cast<int64_t>(number);
}

bool getString(const Json& object, std::string_view name, std::string& value)
{
    const auto found = object.find(name);
    if (found == object.end() || !found->is_string())
    {
        return false;
    }
    value = found->get<std::string>();
    return true;
}

bool convertValue(AttributeType type, const Json& value,
                  AttributeValue& converted)
{
    switch (type)
    {
        case AttributeType::Enumeration:
        case AttributeType::String:
        case AttributeType::Password:
            if (!value.is_string())
            {
                return false;
            }
            converted = value.get<std::string>();
            return true;
        case AttributeType::Integer:
        {
            std::optional<int64_t> number = getInteger(value);
            if (!number)
            {
                return false;
            }
            converted = *number;
            return true;
        }
        case AttributeType::Boolean:
            if (!value.is_boolean())
            {
                return false;
            }
            converted = static_cast<int64_t>(value.get<bool>());
            return true;
    }
    return false;
}

std::optional<AttributeType> parseType(std::string_view type)
{
    if (type == "Enumeration")
    {
        return AttributeType::Enumeration;
    }
    if (type == "String")
    {
        return AttributeType::String;
    }
    if (type == "Password")
    {
        return AttributeType::Password;
    }
    if (type == "Integer")
    {
        return AttributeType::Integer;
    }
    if (type == "Boolean")
    {
        return AttributeType::Boolean;
    }
    return std::nullopt;
}

bool addIntegerOption(const Json& attribute, std::string_view name,
                      BoundType bound, int64_t fallback,
                      AttributeOptions& options)
{
    int64_t value = fallback;
    const auto found = attribute.find(name);
    if (found != attribute.end())
    {
        std::optional<int64_t> parsed = getInteger(*found);
        if (!parsed)
        {
            return false;
        }
        value = *parsed;
    }
    options.emplace_back(bound, value, std::string(name));
    return true;
}

bool parseOptions(const Json& attribute, AttributeType type,
                  AttributeOptions& options, std::string& error)
{
    if (type == AttributeType::Enumeration)
    {
        const auto values = attribute.find("Value");
        if (values == attribute.end() || !values->is_array() ||
            values->empty())
        {
            error = "enumeration attribute has no Value options";
            return false;
        }
        for (const Json& value : *values)
        {
            std::string valueName;
            if (!value.is_object() ||
                !getString(value, "ValueName", valueName))
            {
                error = "enumeration option has no string ValueName";
                return false;
            }
            std::string displayName = valueName;
            const auto display = value.find("ValueDisplayName");
            if (display != value.end())
            {
                if (!display->is_string())
                {
                    error =
                        "enumeration option ValueDisplayName is not a string";
                    return false;
                }
                displayName = display->get<std::string>();
            }
            options.emplace_back(BoundType::OneOf, std::move(valueName),
                                 std::move(displayName));
        }
        return true;
    }

    if (type == AttributeType::String || type == AttributeType::Password)
    {
        if (!addIntegerOption(attribute, "MinLength",
                              BoundType::MinStringLength, 0, options) ||
            !addIntegerOption(attribute, "MaxLength",
                              BoundType::MaxStringLength,
                              defaultMaxStringLength, options))
        {
            error = "string attribute has an invalid length bound";
            return false;
        }
        return true;
    }

    if (type == AttributeType::Integer)
    {
        if (!addIntegerOption(attribute, "LowerBound", BoundType::LowerBound,
                              std::numeric_limits<int64_t>::min(), options) ||
            !addIntegerOption(attribute, "UpperBound", BoundType::UpperBound,
                              std::numeric_limits<int64_t>::max(), options) ||
            !addIntegerOption(attribute, "ScalarIncrement",
                              BoundType::ScalarIncrement, 1, options))
        {
            error = "integer attribute has an invalid bound";
            return false;
        }
    }
    return true;
}

} // namespace

bool parse(const std::string& registryJson, const std::string& currentJson,
           BaseTable& table, std::string& registryId, std::string& error)
{
    table.clear();
    registryId.clear();
    error.clear();

    const Json registry = Json::parse(registryJson, nullptr, false);
    const Json current = Json::parse(currentJson, nullptr, false);
    if (!registry.is_object() || !current.is_object())
    {
        error = "BIOS registry or current settings is not a JSON object";
        return false;
    }
    if (!getString(registry, "Id", registryId))
    {
        error = "BIOS registry has no string Id";
        return false;
    }

    const auto currentRegistry = current.find("AttributeRegistry");
    if (currentRegistry == current.end() || !currentRegistry->is_string() ||
        currentRegistry->get<std::string>() != registryId)
    {
        error = "current BIOS settings references a different registry";
        return false;
    }
    const auto currentAttributes = current.find("Attributes");
    if (currentAttributes == current.end() || !currentAttributes->is_object())
    {
        error = "current BIOS settings has no Attributes object";
        return false;
    }

    const auto registryEntries = registry.find("RegistryEntries");
    if (registryEntries == registry.end() || !registryEntries->is_object())
    {
        error = "BIOS registry has no RegistryEntries object";
        return false;
    }
    const auto attributes = registryEntries->find("Attributes");
    if (attributes == registryEntries->end() || !attributes->is_array() ||
        attributes->empty() || attributes->size() > maxAttributes)
    {
        error = "BIOS registry Attributes array has an invalid size";
        return false;
    }

    for (const Json& attribute : *attributes)
    {
        std::string name;
        std::string typeName;
        if (!attribute.is_object() ||
            !getString(attribute, "AttributeName", name) ||
            name.empty() || name.size() > maxAttributeName ||
            !getString(attribute, "Type", typeName))
        {
            error = "BIOS registry contains an invalid attribute";
            return false;
        }
        const std::optional<AttributeType> type = parseType(typeName);
        if (!type)
        {
            error = "BIOS attribute " + name + " has unsupported type " +
                    typeName;
            return false;
        }

        const auto currentValue = currentAttributes->find(name);
        if (currentValue == currentAttributes->end())
        {
            error = "BIOS attribute " + name + " has no current value";
            return false;
        }
        AttributeValue convertedCurrent;
        if (!convertValue(*type, *currentValue, convertedCurrent))
        {
            error = "BIOS attribute " + name +
                    " has a current value of the wrong type";
            return false;
        }

        AttributeValue convertedDefault = convertedCurrent;
        const auto defaultValue = attribute.find("DefaultValue");
        if (defaultValue != attribute.end() &&
            !convertValue(*type, *defaultValue, convertedDefault))
        {
            error = "BIOS attribute " + name +
                    " has a default value of the wrong type";
            return false;
        }

        bool readOnly = false;
        const auto readOnlyValue = attribute.find("ReadOnly");
        if (readOnlyValue != attribute.end())
        {
            if (!readOnlyValue->is_boolean())
            {
                error = "BIOS attribute " + name +
                        " has a non-boolean ReadOnly value";
                return false;
            }
            readOnly = readOnlyValue->get<bool>();
        }

        std::string displayName = name;
        const auto display = attribute.find("DisplayName");
        if (display != attribute.end())
        {
            if (!display->is_string())
            {
                error = "BIOS attribute " + name +
                        " has a non-string DisplayName";
                return false;
            }
            displayName = display->get<std::string>();
        }
        std::string description;
        const auto help = attribute.find("HelpText");
        if (help != attribute.end())
        {
            if (!help->is_string())
            {
                error = "BIOS attribute " + name +
                        " has a non-string HelpText";
                return false;
            }
            description = help->get<std::string>();
        }

        AttributeOptions options;
        if (!parseOptions(attribute, *type, options, error))
        {
            error = "BIOS attribute " + name + ": " + error;
            return false;
        }

        const auto [unused, inserted] = table.emplace(
            name, std::make_tuple(*type, readOnly, std::move(displayName),
                                  std::move(description), std::string{},
                                  std::move(convertedCurrent),
                                  std::move(convertedDefault),
                                  std::move(options)));
        if (!inserted)
        {
            error = "BIOS registry contains duplicate attribute " + name;
            return false;
        }
    }
    return true;
}

} // namespace ami::bios
