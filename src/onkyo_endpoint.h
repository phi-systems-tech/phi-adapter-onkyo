#pragma once

// Addressing: which port, which host, and what the thing at the other end is
// called. Shared by the instance and by the factory's probe, which used to
// answer these questions separately and did not always agree.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "onkyo_json.h"

namespace phicore::onkyo::ipc {

/// The ISCP port a receiver listens on when nothing else is configured.
inline constexpr std::uint16_t kDefaultIscpPort = 60128;

/// 0 for anything outside 1..65535, which callers read as "not configured".
std::uint16_t normalizedPort(int value);

/// Configured control port, or the ISCP default.
std::uint16_t resolvedControlPort(std::uint16_t adapterPortValue);

/// Whether the text is an address rather than a name - no lookup involved.
bool isIpLiteral(std::string_view text);

/// Model name inferred from an mDNS-style identifier, or empty when unclear.
std::string inferModelFromIdentifier(std::string_view raw);

/**
 * @brief Candidate hosts from a factory action's params, addresses first.
 *
 * Addresses first because they cost nothing to try, and a name that resolves
 * to the same machine then costs nothing either.
 */
std::vector<std::string> resolveProbeHosts(const Json &params);

/// `iscpPort` from the action params; 0 when absent or unusable.
std::uint16_t resolveProbePort(const Json &params);

} // namespace phicore::onkyo::ipc
