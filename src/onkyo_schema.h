#pragma once

// Adapter identity and the config schema phi-ui renders. Separate from the
// sidecar because it is data, not behaviour: it describes what the adapter
// offers, and nothing here talks to a receiver.

#include <QHash>
#include <QJsonObject>
#include <QString>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::onkyo::ipc {

inline constexpr const char kPluginType[] = "onkyo";

inline constexpr const char kChannelPower[] = "power";
inline constexpr const char kChannelVolume[] = "volume";
inline constexpr const char kChannelMute[] = "mute";
inline constexpr const char kChannelInput[] = "input";
inline constexpr const char kChannelConnectivity[] = "connectivity";

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
phicore::adapter::v1::JsonText configSchemaJson(const QHash<QString, QString> &labels);

/// SDK option list -> `{value,label}` array, for schema-shaped channel metadata.
QJsonArray optionsToChoiceJson(const phicore::adapter::v1::AdapterConfigOptionList &options);

} // namespace phicore::onkyo::ipc
