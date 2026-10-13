#pragma once

#include <optional>
#include <string_view>

#include "phi/adapter/v1/types.h"

namespace phicore::onkyo::ipc {

/// Maps the generic player controls supported by an Onkyo/Pioneer NET player
/// to their eISCP payload. URL playback and position-based seeking are not part
/// of this receiver protocol and deliberately have no mapping.
std::optional<std::string_view> mediaPlayerCommand(
    phicore::adapter::v1::MediaPlayerAction action) noexcept;

} // namespace phicore::onkyo::ipc
