#pragma once

// The input-selector vocabulary: what an SLI code is, and what it is called.
// Pure over strings, no adapter state, so the vocabulary can be argued about
// without a receiver in the room.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "onkyo_json.h"

namespace phicore::onkyo::ipc {

/**
 * @brief Canonical form of an SLI code, or empty.
 *
 * Accepts `SLI05`, `sli 5`, `5`, `05` and answers `05`. Anything that is not
 * two hexadecimal digits after that answers empty, and callers read empty as
 * "not a code".
 *
 * It used to let `TV` through, which is how the adapter came to accept an
 * input it could never confirm: `SLITV` went out on the wire, no receiver
 * answered it, and the reader - which has always required two hex digits -
 * would not have recognised the answer either. A code is what a receiver
 * speaks or it is not a code.
 */
std::string normalizeSliCode(std::string_view raw);

/// Configured label, or a `SLI <code>` fallback; empty for an unusable code.
std::string formatSliDisplayLabel(std::string_view code, std::string_view mappedLabel);

/// Codes from a scalar or an array, normalized, deduplicated, in the order
/// they were written - which is the order the user put them in.
std::vector<std::string> normalizeActiveSliCodes(const Json &value);

/// Code -> label from the factory's `sliLabels` static config object. Ordered,
/// so anything derived from it comes out the same way twice.
std::map<std::string, std::string> loadConfiguredSliLabels(const Json &staticConfig);

} // namespace phicore::onkyo::ipc
