#include "onkyo_trace.h"

#include <iostream>

#include <QDateTime>

namespace phicore::onkyo::ipc {

namespace {

constexpr bool kTraceEnabled = false;
constexpr bool kTimingLogsEnabled = true;

} // namespace

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void trace(const QString &message)
{
    if (!kTraceEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc: " << message.toStdString() << '\n';
}

void timingLog(const QString &message)
{
    if (!kTimingLogsEnabled)
        return;
    std::cerr << "[" << nowMs() << "] onkyo-ipc[timing]: " << message.toStdString() << '\n';
}

} // namespace phicore::onkyo::ipc
