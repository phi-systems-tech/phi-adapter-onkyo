#pragma once

// Diagnostic side channel for the onkyo sidecar. These are protocol and timing
// traces - the poll cycles and command round trips - which the contract calls
// Trace: off unless somebody asked for them.
//
// They used to go to stderr, where phi-core stamps every line Warn and no
// filter applies, and a flag in this file was the only way to turn them off.
// Now they go where the instance sends them: `setTraceSink()` is handed the
// instance's log(), so the adapter's "Logs" switch decides, and the lines are
// levelled like everything else. Without a sink they are dropped, which is
// what happens before an instance exists.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace phicore::onkyo::ipc {

std::int64_t nowMs();

/// Where the traces go. Set by the instance while it runs, cleared when it
/// stops; both calls belong to the loop thread, as everything else here does.
void setTraceSink(std::function<void(std::string_view)> sink);

/// Verbose protocol trace; off unless the adapter is built with tracing on.
void trace(std::string_view message);

/// Command/poll timing trace, used to spot stalls in the field: Trace, so it
/// costs nothing until the adapter's log switch is on.
void timingLog(std::string_view message);

} // namespace phicore::onkyo::ipc
