#include "onkyo_state.h"

#include <algorithm>
#include <cmath>

namespace phicore::onkyo::ipc {

void ReceiverState::setVolumeMaxRaw(int maxRaw)
{
    m_volumeMaxRaw = std::clamp(maxRaw, 1, 500);
}

std::int64_t ReceiverState::percentForRaw(int raw) const
{
    const int clamped = std::clamp(raw, 0, m_volumeMaxRaw);
    const double percent = (static_cast<double>(clamped) / m_volumeMaxRaw) * 100.0;
    return static_cast<std::int64_t>(std::llround(percent));
}

int ReceiverState::rawForPercent(double percent) const
{
    const double clamped = std::clamp(percent, 0.0, 100.0);
    const long long raw = std::llround((clamped / 100.0) * m_volumeMaxRaw);
    return static_cast<int>(std::clamp<long long>(raw, 0, m_volumeMaxRaw));
}

std::optional<ChannelReport> ReceiverState::applyPower(bool on)
{
    m_power = on ? PowerState::On : PowerState::Off;
    if (m_reportedPower.has_value() && *m_reportedPower == on)
        return std::nullopt;
    m_reportedPower = on;
    ChannelReport report;
    report.kind = StatusKind::Power;
    report.flag = on;
    return report;
}

std::optional<ChannelReport> ReceiverState::applyMute(bool muted)
{
    if (m_reportedMute.has_value() && *m_reportedMute == muted)
        return std::nullopt;
    m_reportedMute = muted;
    ChannelReport report;
    report.kind = StatusKind::Mute;
    report.flag = muted;
    return report;
}

std::optional<ChannelReport> ReceiverState::applyVolumePercent(std::int64_t percent)
{
    if (m_reportedVolume.has_value() && *m_reportedVolume == percent)
        return std::nullopt;
    m_reportedVolume = percent;
    ChannelReport report;
    report.kind = StatusKind::Volume;
    report.volumePercent = percent;
    return report;
}

std::optional<ChannelReport> ReceiverState::applyInput(const std::string &code)
{
    if (code.empty())
        return std::nullopt;
    m_lastInput = code;
    if (m_reportedInput.has_value() && *m_reportedInput == code)
        return std::nullopt;
    m_reportedInput = code;
    ChannelReport report;
    report.kind = StatusKind::Input;
    report.inputCode = code;
    return report;
}

std::vector<ChannelReport> ReceiverState::apply(const std::vector<Status> &statuses)
{
    std::vector<ChannelReport> reports;
    for (const Status &status : statuses) {
        std::optional<ChannelReport> report;
        switch (status.kind) {
        case StatusKind::Power:
            report = applyPower(status.flag);
            break;
        case StatusKind::Mute:
            report = applyMute(status.flag);
            break;
        case StatusKind::Volume:
            report = applyVolumePercent(percentForRaw(status.raw));
            break;
        case StatusKind::Input:
            report = applyInput(status.code);
            break;
        }
        if (report)
            reports.push_back(*report);
    }
    return reports;
}

void ReceiverState::forget()
{
    m_power = PowerState::Unknown;
    m_reportedPower.reset();
    m_reportedMute.reset();
    m_reportedVolume.reset();
    m_reportedInput.reset();
    // The last code is kept: it is the answer "probe current input" gives when
    // the receiver has gone quiet, and forgetting it turns a stale answer into
    // no answer at all.
}

} // namespace phicore::onkyo::ipc
