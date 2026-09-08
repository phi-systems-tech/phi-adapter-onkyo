#pragma once

// The input catalogue: which selector codes this receiver has, and what each
// one is called. Three sources, in order - the label the user typed, the
// default the factory read from its static config, and `SLI <code>` when there
// is neither.

#include <map>
#include <string>
#include <vector>

#include "phi/adapter/v1/schema.h"

#include "onkyo_json.h"

namespace phicore::onkyo::ipc {

class InputCatalog
{
public:
    /// The factory's `sliLabels`, read once at bootstrap.
    void setDefaults(std::map<std::string, std::string> defaults);

    /// Re-reads `activeSliCodes` and every `inputLabel_<code>` from the
    /// instance's meta.
    void applyMeta(const Json &meta);

    /// The codes this receiver offers, in the order they were configured.
    const std::vector<std::string> &activeCodes() const { return m_active; }

    /// Label for one code, always something a person can read.
    std::string label(const std::string &code) const;

    /// The default the factory knows for a code, or empty.
    std::string defaultLabel(const std::string &code) const;

    /**
     * @brief Choices for the input channel.
     *
     * The configured codes, or - while nothing is configured yet - everything
     * the factory has a name for, so a fresh instance still offers something.
     */
    phicore::adapter::v1::AdapterConfigOptionList choices() const;

    /// A code the user may have typed as its label instead; empty if unknown.
    std::string codeForLabel(const std::string &text) const;

private:
    std::map<std::string, std::string> m_defaults;
    std::map<std::string, std::string> m_labels;
    std::vector<std::string> m_active;
};

} // namespace phicore::onkyo::ipc
