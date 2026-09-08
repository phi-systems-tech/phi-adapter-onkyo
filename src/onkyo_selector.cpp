#include "onkyo_selector.h"

#include <algorithm>

#include "phi/runtime/str.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

namespace {

bool isHexPair(std::string_view s)
{
    if (s.size() != 2)
        return false;
    for (const char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!hex)
            return false;
    }
    return true;
}

/// A JSON entry as the text of a code: strings as written, numbers as digits.
std::string codeText(const Json &entry)
{
    if (entry.is_string())
        return entry.get<std::string>();
    if (entry.is_number_integer())
        return str::number(entry.get<long long>());
    if (entry.is_number_float())
        return str::number(static_cast<long long>(entry.get<double>()));
    return {};
}

} // namespace

std::string normalizeSliCode(std::string_view raw)
{
    std::string code = str::toUpper(str::trimmed(raw));
    if (str::startsWithIgnoreCase(code, "SLI"))
        code = str::trimmed(std::string_view(code).substr(3));
    code.erase(std::remove(code.begin(), code.end(), ' '), code.end());
    if (code.empty())
        return {};
    // A single digit is the receiver's own shorthand for a leading zero.
    if (code.size() == 1)
        code.insert(code.begin(), '0');
    if (!isHexPair(code))
        return {};
    return code;
}

std::string formatSliDisplayLabel(std::string_view code, std::string_view mappedLabel)
{
    const std::string label = str::trimmed(mappedLabel);
    if (!label.empty())
        return label;
    const std::string normalized = normalizeSliCode(code);
    if (normalized.empty())
        return {};
    return "SLI " + normalized;
}

std::vector<std::string> normalizeActiveSliCodes(const Json &value)
{
    std::vector<std::string> codes;
    const auto append = [&codes](const std::string &raw) {
        const std::string code = normalizeSliCode(raw);
        if (code.empty())
            return;
        if (std::find(codes.begin(), codes.end(), code) != codes.end())
            return;
        codes.push_back(code);
    };

    if (value.is_array()) {
        for (const Json &entry : value)
            append(codeText(entry));
        return codes;
    }
    append(codeText(value));
    return codes;
}

std::map<std::string, std::string> loadConfiguredSliLabels(const Json &staticConfig)
{
    std::map<std::string, std::string> map;
    if (!staticConfig.is_object())
        return map;
    const auto labels = staticConfig.find("sliLabels");
    if (labels == staticConfig.end() || !labels->is_object())
        return map;

    for (const auto &entry : labels->items()) {
        const std::string code = normalizeSliCode(entry.key());
        if (code.empty())
            continue;
        if (!entry.value().is_string())
            continue;
        const std::string label = str::trimmed(entry.value().get<std::string>());
        if (label.empty())
            continue;
        map.insert_or_assign(code, label);
    }
    return map;
}

} // namespace phicore::onkyo::ipc
