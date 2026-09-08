#pragma once

// Diagnostic side channel for the onkyo sidecar: plain stderr, which phi-core
// forwards as adapter logs. Kept separate from the structured PHI_LOG_* path
// because these are protocol/timing traces, compiled out by a flag rather than
// filtered by level.

#include <cstdint>
#include <string>
#include <string_view>

namespace phicore::onkyo::ipc {

std::int64_t nowMs();

/// Verbose protocol trace; off unless the adapter is built with tracing on.
void trace(std::string_view message);

/// Command/poll timing trace; on by default, used to spot stalls in the field.
void timingLog(std::string_view message);

} // namespace phicore::onkyo::ipc
