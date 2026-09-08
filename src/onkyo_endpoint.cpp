#include "onkyo_endpoint.h"

#include <algorithm>
#include <array>

#include <arpa/inet.h>

#include "phi/runtime/str.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

namespace {

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/// Drops one trailing `-<4..12 hex>` group, which is how mDNS names carry the
/// unit's serial suffix.
std::string dropSerialSuffix(const std::string &name)
{
    const std::size_t dash = name.rfind('-');
    if (dash == std::string::npos || dash == 0)
        return name;
    const std::string tail = name.substr(dash + 1);
    if (tail.size() < 4 || tail.size() > 12)
        return name;
    if (!std::all_of(tail.begin(), tail.end(), isHexDigit))
        return name;
    return name.substr(0, dash);
}

} // namespace

std::uint16_t normalizedPort(int value)
{
    if (value <= 0 || value > 65535)
        return 0;
    return static_cast<std::uint16_t>(value);
}

std::uint16_t resolvedControlPort(std::uint16_t adapterPortValue)
{
    const std::uint16_t adapterPort = normalizedPort(static_cast<int>(adapterPortValue));
    return adapterPort == 0 ? kDefaultIscpPort : adapterPort;
}

bool isIpLiteral(std::string_view text)
{
    const std::string value(text);
    std::array<unsigned char, 16> scratch{};
    if (::inet_pton(AF_INET, value.c_str(), scratch.data()) == 1)
        return true;
    return ::inet_pton(AF_INET6, value.c_str(), scratch.data()) == 1;
}

std::string inferModelFromIdentifier(std::string_view raw)
{
    std::string trimmed = str::trimmed(raw);
    if (trimmed.empty())
        return {};

    const std::size_t portIndex = trimmed.rfind(':');
    if (portIndex != std::string::npos && portIndex > 0)
        trimmed = trimmed.substr(0, portIndex);

    if (str::endsWithIgnoreCase(trimmed, ".local"))
        trimmed = trimmed.substr(0, trimmed.size() - 6);

    static constexpr std::string_view kVendors[] = {"Pioneer", "Onkyo"};
    std::string rest;
    bool matched = false;
    for (const std::string_view vendor : kVendors) {
        if (!str::startsWithIgnoreCase(trimmed, vendor))
            continue;
        rest = trimmed.substr(vendor.size());
        matched = true;
        break;
    }
    if (!matched)
        return {};
    if (!rest.empty() && (rest.front() == '-' || rest.front() == '_' || rest.front() == ' '))
        rest.erase(rest.begin());

    const std::string model = str::trimmed(dropSerialSuffix(rest));
    // A model number has a number in it. Without one this is a vendor name and
    // a word, which says nothing the manufacturer field does not say already.
    const bool hasDigit = std::any_of(model.begin(), model.end(),
                                      [](char c) { return c >= '0' && c <= '9'; });
    if (model.empty() || !hasDigit)
        return {};
    return model;
}

std::vector<std::string> resolveProbeHosts(const Json &params)
{
    std::vector<std::string> addresses;
    std::vector<std::string> names;

    const auto consider = [&addresses, &names](const std::string &raw) {
        const std::string value = str::trimmed(raw);
        if (value.empty())
            return;
        std::vector<std::string> &bucket = isIpLiteral(value) ? addresses : names;
        if (std::find(bucket.begin(), bucket.end(), value) == bucket.end())
            bucket.push_back(value);
    };

    const Json adapter = params.is_object() && params.contains("adapter")
        ? params.at("adapter")
        : Json::object();

    consider(jsonString(params, "ip"));
    consider(jsonString(adapter, "ip"));
    consider(jsonString(params, "host"));
    consider(jsonString(adapter, "host"));

    addresses.insert(addresses.end(), names.begin(), names.end());
    return addresses;
}

std::uint16_t resolveProbePort(const Json &params)
{
    return normalizedPort(jsonInt(params, "iscpPort", 0));
}

} // namespace phicore::onkyo::ipc
