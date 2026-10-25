#include "onkyo_inputs.h"

#include "phi/runtime/str.h"

#include "onkyo_selector.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

void InputCatalog::setDefaults(std::map<std::string, std::string> defaults)
{
    m_defaults = std::move(defaults);
}

void InputCatalog::applyMeta(const Json &meta)
{
    m_labels.clear();
    m_active.clear();
    if (!meta.is_object())
        return;

    const auto active = meta.find("activeSliCodes");
    m_active = normalizeActiveSliCodes(active == meta.end() ? Json() : *active);

    for (const std::string &code : m_active) {
        const std::string custom = str::trimmed(jsonString(meta, "inputLabel_" + code));
        if (!custom.empty()) {
            m_labels.insert_or_assign(code, custom);
            continue;
        }
        const std::string fallback = defaultLabel(code);
        m_labels.insert_or_assign(code, fallback.empty() ? "SLI " + code : fallback);
    }
}

std::string InputCatalog::defaultLabel(const std::string &code) const
{
    const auto it = m_defaults.find(code);
    return it == m_defaults.end() ? std::string() : str::trimmed(it->second);
}

std::string InputCatalog::label(const std::string &code) const
{
    const auto it = m_labels.find(code);
    if (it != m_labels.end() && !it->second.empty())
        return it->second;
    return formatSliDisplayLabel(code, defaultLabel(code));
}

v1::AdapterConfigOptionList InputCatalog::choices() const
{
    std::vector<std::string> keys = m_active;
    if (keys.empty()) {
        // Nothing configured yet. Everything the factory has a name for beats
        // an empty selector, and the codes come out sorted because a map is
        // sorted - the old code took the keys of a QHash, so the same
        // configuration produced a different order every run.
        for (const auto &entry : m_defaults)
            keys.push_back(entry.first);
    }

    v1::AdapterConfigOptionList options;
    options.reserve(keys.size());
    for (const std::string &code : keys) {
        options.emplace_back(code, label(code));
    }
    return options;
}

std::string InputCatalog::codeForLabel(const std::string &text) const
{
    const std::string wanted = str::toLower(str::trimmed(text));
    if (wanted.empty())
        return {};
    for (const auto &entry : m_labels) {
        if (str::toLower(entry.second) == wanted)
            return entry.first;
    }
    for (const auto &entry : m_defaults) {
        if (str::toLower(entry.second) == wanted)
            return entry.first;
    }
    return {};
}

} // namespace phicore::onkyo::ipc
