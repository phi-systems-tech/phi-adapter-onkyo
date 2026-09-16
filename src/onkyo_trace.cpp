#include "onkyo_trace.h"

#include <chrono>
#include <utility>

namespace phicore::onkyo::ipc {

namespace {

constexpr bool kTraceEnabled = false;

std::function<void(std::string_view)> g_sink;

} // namespace

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void setTraceSink(std::function<void(std::string_view)> sink)
{
    g_sink = std::move(sink);
}

void trace(std::string_view message)
{
    if (!kTraceEnabled || !g_sink)
        return;
    g_sink(message);
}

void timingLog(std::string_view message)
{
    if (g_sink)
        g_sink(message);
}

} // namespace phicore::onkyo::ipc
