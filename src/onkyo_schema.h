#pragma once

// Adapter identity and the config schema phi-ui renders. Data, not behaviour:
// it describes what the adapter offers, and nothing here talks to a receiver.

#include <map>
#include <string>

#include "phi/adapter/sdk/sidecar.h"

#include "onkyo_json.h"

namespace phicore::onkyo::ipc {

inline constexpr const char kPluginType[] = "onkyo";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();

phicore::adapter::v1::AdapterCapabilities capabilities();

/**
 * @brief Config schema for the given SLI label map.
 *
 * Unlike most adapters the schema is not static: the input selector's choices
 * and the per-code label fields are generated from the labels the factory read
 * at bootstrap.
 */
phicore::adapter::v1::JsonText configSchemaJson(const std::map<std::string, std::string> &labels);

/// SDK option list -> `{value,label}` array, for schema-shaped channel metadata.
Json optionsToChoiceJson(const phicore::adapter::v1::AdapterConfigOptionList &options);

} // namespace phicore::onkyo::ipc
