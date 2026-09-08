#include "onkyo_trace.h"

#include <chrono>
#include <iostream>

namespace phicore::onkyo::ipc {

namespace {

constexpr bool kTraceEnabled = false;
constexpr bool kTimingLogsEnabled = true;

} // namespace

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void trace(std::string_view message)
{
    if (!kTraceEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc: " << message << '\n';
}

void timingLog(std::string_view message)
{
    if (!kTimingLogsEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc[timing]: " << message << '\n';
}

} // namespace phicore::onkyo::ipc
