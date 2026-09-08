#pragma once

// What the receiver last said, and whether saying it again is news.
//
// Deduplication used to be four member pairs and four near-identical guards
// scattered through the instance. It is one object here, which is why it can
// be handed a sequence of frames in a test and asked what it would have
// reported.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "onkyo_frames.h"

namespace phicore::onkyo::ipc {

enum class PowerState {
    Unknown,
    Off,
    On,
};

/// One channel value worth sending to phi-core.
struct ChannelReport {
    StatusKind kind = StatusKind::Power;
    bool flag = false;
    std::int64_t volumePercent = 0;
    std::string inputCode;
};

class ReceiverState
{
public:
    /// Raw scale the receiver counts volume on; 1..500, default 160.
    void setVolumeMaxRaw(int maxRaw);
    int volumeMaxRaw() const { return m_volumeMaxRaw; }

    /// Percentage for a raw level, and the raw level for a percentage.
    std::int64_t percentForRaw(int raw) const;
    int rawForPercent(double percent) const;

    PowerState power() const { return m_power; }
    const std::string &lastInputCode() const { return m_lastInput; }

    /**
     * @brief Folds what the receiver said into the state.
     * @return only the values that are news; unchanged ones are not reported.
     */
    std::vector<ChannelReport> apply(const std::vector<Status> &statuses);

    /// The same for a value this adapter just wrote, so an echo does not report
    /// it twice.
    std::optional<ChannelReport> applyPower(bool on);
    std::optional<ChannelReport> applyMute(bool muted);
    std::optional<ChannelReport> applyVolumePercent(std::int64_t percent);
    std::optional<ChannelReport> applyInput(const std::string &code);

    /**
     * @brief Forgets everything the receiver said.
     *
     * Used when connectivity is lost or the endpoint changes: what was true of
     * the old connection says nothing about the new one, and the next poll must
     * be allowed to report the same values again.
     */
    void forget();

private:
    int m_volumeMaxRaw = 160;
    PowerState m_power = PowerState::Unknown;
    std::optional<bool> m_reportedPower;
    std::optional<bool> m_reportedMute;
    std::optional<std::int64_t> m_reportedVolume;
    std::optional<std::string> m_reportedInput;
    std::string m_lastInput;
};

} // namespace phicore::onkyo::ipc
